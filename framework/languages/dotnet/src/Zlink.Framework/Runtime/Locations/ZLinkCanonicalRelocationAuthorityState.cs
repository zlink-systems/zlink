using System.Buffers.Binary;
using System.Text;

namespace Zlink.Framework.Runtime.Locations;

internal sealed record ZLinkCanonicalRelocationAuthorityState(
    ulong RelocationHigh,
    ulong RelocationLow,
    ulong TargetAttemptGeneration,
    string SourceNodeRid,
    ulong SourceNodeGeneration,
    string SourceOwnerId,
    ulong SourceOwnerLeaseGeneration,
    string TargetNodeRid,
    ulong TargetNodeGeneration,
    string TargetOwnerId,
    ulong TargetOwnerLeaseGeneration,
    ulong ReservationGeneration,
    string CoordinatorOwnerId,
    ulong CoordinatorLeaseGeneration,
    string CoordinatorNodeRid,
    ulong CoordinatorNodeGeneration,
    byte Phase,
    string RelocationReference,
    uint RelocationChecksumCrc32c,
    long ApplicationVersion,
    byte SourceCleanupState)
{
    internal ulong AggregateGeneration { get; init; }
}

internal sealed record ZLinkCanonicalRelocationAuthorityProjection(
    ulong RelocationHigh,
    ulong RelocationLow,
    ulong TargetAttemptGeneration,
    string TargetOwnerId,
    ulong TargetOwnerLeaseGeneration,
    byte Phase,
    string RelocationReference,
    uint RelocationChecksumCrc32c,
    long ApplicationVersion,
    uint TerminalCompletionCount,
    uint PendingRelayCount,
    byte SourceCleanupState,
    ReadOnlyMemory<byte> SteadyAuthorityPayload,
    ZLinkCanonicalRelocationAuthorityState State)
{
    internal ulong AggregateGeneration { get; init; }
}

internal static class ZLinkCanonicalRelocationAuthorityStateCodec
{
    private static ReadOnlySpan<byte> Magic => "ZLAU"u8;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    internal static bool TryRead(
        ReadOnlySpan<byte> authorityPayload,
        out ZLinkCanonicalRelocationAuthorityProjection projection)
    {
        projection = null!;
        try
        {
            var source = new Reader(authorityPayload);
            if (!source.Bytes(4).SequenceEqual(Magic) || source.U8() != 1)
                return false;
            var flags = source.U16();
            var body = source.Bytes(source.U32AsInt()).ToArray();
            var checksumOffset = source.Offset;
            if (source.U32() != ZLinkCrc32C.Compute(authorityPayload[..checksumOffset])
                || !source.End)
                return false;
            var bodyReader = new Reader(body);
            _ = bodyReader.U8();
            _ = bodyReader.U8();
            _ = bodyReader.Bytes(bodyReader.U16());
            _ = bodyReader.Text8();
            _ = bodyReader.U64();
            _ = bodyReader.Text8();
            _ = bodyReader.Text8();
            _ = bodyReader.U64();
            var slotStart = bodyReader.Offset;
            if (bodyReader.U8() != 1)
                return false;
            var relocation = new Reader(bodyReader.Bytes(bodyReader.U32AsInt()));
            var high = relocation.U64();
            var low = relocation.U64();
            var attempt = relocation.U64();
            var sourceNodeRid = relocation.Text8();
            var sourceNodeGeneration = relocation.U64();
            var sourceOwner = relocation.Text8();
            var sourceOwnerLease = relocation.U64();
            var targetNodeRid = relocation.OptionalText8();
            var targetNodeGeneration = relocation.U64();
            var targetOwner = relocation.OptionalText8();
            var targetLease = relocation.U64();
            var reservationGeneration = relocation.U64();
            var coordinatorOwner = relocation.Text8();
            var coordinatorLease = relocation.U64();
            var coordinatorNodeRid = relocation.Text8();
            var coordinatorNodeGeneration = relocation.U64();
            var phase = relocation.U8();
            var pointerPresence = relocation.U8();
            if (pointerPresence > 1)
                return false;
            var pointer = new Reader(relocation.Bytes(relocation.U16()));
            var reference = string.Empty;
            var checksum = 0U;
            if (pointerPresence == 1)
            {
                reference = pointer.Text16();
                checksum = pointer.U32();
            }
            if (!pointer.End) return false;
            var applicationVersion = relocation.I64();
            var progressCount = relocation.U32AsInt();
            _ = relocation.Bytes(checked(progressCount * 3 * sizeof(ulong)));
            var terminalCount = relocation.U32();
            var pendingCount = relocation.U32();
            var cleanup = relocation.U8();
            var aggregateGeneration = 0UL;
            if (!relocation.End)
            {
                if (relocation.U8() != 1)
                    return false;
                aggregateGeneration = relocation.U64();
            }
            if (!relocation.End)
                return false;
            var slotEnd = bodyReader.Offset;
            using var steadyBody = new MemoryStream();
            steadyBody.Write(body.AsSpan(0, slotStart));
            steadyBody.WriteByte(0);
            WriteU32(steadyBody, 0);
            steadyBody.Write(body.AsSpan(slotEnd));
            using var steady = new MemoryStream();
            steady.Write(Magic);
            steady.WriteByte(1);
            WriteU16(steady, flags);
            WriteU32(steady, checked((uint)steadyBody.Length));
            steadyBody.Position = 0;
            steadyBody.CopyTo(steady);
            WriteU32(steady, ZLinkCrc32C.Compute(steady.ToArray()));
            var state = new ZLinkCanonicalRelocationAuthorityState(
                high, low, attempt,
                sourceNodeRid, sourceNodeGeneration,
                sourceOwner, sourceOwnerLease,
                targetNodeRid, targetNodeGeneration,
                targetOwner, targetLease,
                reservationGeneration,
                coordinatorOwner, coordinatorLease,
                coordinatorNodeRid, coordinatorNodeGeneration,
                phase, reference, checksum, applicationVersion, cleanup)
            {
                AggregateGeneration = aggregateGeneration
            };
            projection = new ZLinkCanonicalRelocationAuthorityProjection(
                high, low, attempt, targetOwner, targetLease, phase,
                reference, checksum, applicationVersion,
                terminalCount, pendingCount, cleanup, steady.ToArray(), state)
            {
                AggregateGeneration = aggregateGeneration
            };
            return true;
        }
        catch (Exception error) when (error is IOException
                                      or OverflowException
                                      or DecoderFallbackException
                                      or ArgumentException)
        {
            return false;
        }
    }

    internal static (uint TerminalCompletionCount, uint PendingRelayCount)
        ReadCounts(ReadOnlySpan<byte> authorityPayload)
    {
        var source = new Reader(authorityPayload);
        if (!source.Bytes(4).SequenceEqual(Magic) || source.U8() != 1)
            throw new InvalidDataException();
        _ = source.U16();
        var body = new Reader(source.Bytes(source.U32AsInt()));
        _ = body.U8();
        _ = body.U8();
        _ = body.Bytes(body.U16());
        _ = body.Text8();
        _ = body.U64();
        _ = body.Text8();
        _ = body.Text8();
        _ = body.U64();
        if (body.U8() != 1)
            throw new InvalidDataException("Canonical authority has no relocation state.");
        var relocation = new Reader(body.Bytes(body.U32AsInt()));
        _ = relocation.U64();
        _ = relocation.U64();
        _ = relocation.U64();
        _ = relocation.Text8();
        _ = relocation.U64();
        _ = relocation.Text8();
        _ = relocation.U64();
        _ = relocation.OptionalText8();
        _ = relocation.U64();
        _ = relocation.OptionalText8();
        _ = relocation.U64();
        _ = relocation.U64();
        _ = relocation.Text8();
        _ = relocation.U64();
        _ = relocation.Text8();
        _ = relocation.U64();
        _ = relocation.U8();
        var pointerPresence = relocation.U8();
        if (pointerPresence > 1)
            throw new InvalidDataException();
        var pointer = relocation.Bytes(relocation.U16());
        if (pointerPresence == 0 && pointer.Length != 0)
            throw new InvalidDataException();
        _ = relocation.U64();
        var progressCount = relocation.U32AsInt();
        _ = relocation.Bytes(checked(progressCount * 3 * sizeof(ulong)));
        return (relocation.U32(), relocation.U32());
    }

    internal static byte[] ReplaceRelocationState(
        ReadOnlySpan<byte> authorityPayload,
        ZLinkCanonicalRelocationAuthorityState state,
        ZLinkRelocationEnvelope? root)
    {
        if (root is not null)
        {
            Span<byte> relocationId = stackalloc byte[16];
            BinaryPrimitives.WriteUInt64BigEndian(
                relocationId[..8], state.RelocationHigh);
            BinaryPrimitives.WriteUInt64BigEndian(
                relocationId[8..], state.RelocationLow);
            if (root.AggregateId != new Guid(relocationId, bigEndian: true))
                throw new ArgumentException(
                    "Canonical authority relocation identity differs from its root.",
                    nameof(root));
        }
        var source = new Reader(authorityPayload);
        if (!source.Bytes(4).SequenceEqual(Magic)
            || source.U8() != 1)
            throw new InvalidDataException("The authority payload is not canonical ZLAU v1.");
        var flags = source.U16();
        var body = source.Bytes(source.U32AsInt()).ToArray();
        var checksumOffset = source.Offset;
        if (source.U32() != ZLinkCrc32C.Compute(authorityPayload[..checksumOffset])
            || !source.End)
            throw new InvalidDataException("The canonical authority checksum is invalid.");

        var bodyReader = new Reader(body);
        _ = bodyReader.U8();
        _ = bodyReader.U8();
        _ = bodyReader.Bytes(bodyReader.U16());
        _ = bodyReader.Text8();
        _ = bodyReader.U64();
        _ = bodyReader.Text8();
        _ = bodyReader.Text8();
        _ = bodyReader.U64();
        var relocationOffset = bodyReader.Offset;
        var presence = bodyReader.U8();
        if (presence > 1)
            throw new InvalidDataException("The authority relocation presence flag is invalid.");
        _ = bodyReader.Bytes(bodyReader.U32AsInt());
        var relocationEnd = bodyReader.Offset;

        var slot = EncodeSlot(state, root);
        using var replacedBody = new MemoryStream();
        replacedBody.Write(body.AsSpan(0, relocationOffset));
        replacedBody.Write(slot);
        replacedBody.Write(body.AsSpan(relocationEnd));
        using var result = new MemoryStream();
        result.Write(Magic);
        result.WriteByte(1);
        WriteU16(result, flags);
        WriteU32(result, checked((uint)replacedBody.Length));
        replacedBody.Position = 0;
        replacedBody.CopyTo(result);
        WriteU32(result, ZLinkCrc32C.Compute(result.ToArray()));
        return result.ToArray();
    }

    internal static byte[] ReplaceSteadyAuthorityPayload(
        ReadOnlySpan<byte> authorityPayload,
        ReadOnlySpan<byte> steadyAuthorityPayload)
    {
        var original = ReadEnvelope(authorityPayload);
        var steady = ReadEnvelope(steadyAuthorityPayload);
        var originalSlot = ReadSlot(original.Body);
        var steadySlot = ReadSlot(steady.Body);
        if (steadySlot.End - steadySlot.Start != 5
            || steady.Body[steadySlot.Start] != 0)
            throw new InvalidDataException(
                "The replacement steady authority contains relocation state.");

        using var body = new MemoryStream();
        body.Write(steady.Body.AsSpan(0, steadySlot.Start));
        body.Write(original.Body.AsSpan(
            originalSlot.Start,
            originalSlot.End - originalSlot.Start));
        body.Write(steady.Body.AsSpan(steadySlot.End));
        using var result = new MemoryStream();
        result.Write(Magic);
        result.WriteByte(1);
        WriteU16(result, steady.Flags);
        WriteU32(result, checked((uint)body.Length));
        body.Position = 0;
        body.CopyTo(result);
        WriteU32(result, ZLinkCrc32C.Compute(result.ToArray()));
        return result.ToArray();
    }

    private static (ushort Flags, byte[] Body) ReadEnvelope(
        ReadOnlySpan<byte> payload)
    {
        var reader = new Reader(payload);
        if (!reader.Bytes(4).SequenceEqual(Magic) || reader.U8() != 1)
            throw new InvalidDataException(
                "The authority payload is not canonical ZLAU v1.");
        var flags = reader.U16();
        var body = reader.Bytes(reader.U32AsInt()).ToArray();
        var checksumOffset = reader.Offset;
        if (reader.U32() != ZLinkCrc32C.Compute(payload[..checksumOffset])
            || !reader.End)
            throw new InvalidDataException(
                "The canonical authority checksum is invalid.");
        return (flags, body);
    }

    private static (int Start, int End) ReadSlot(ReadOnlySpan<byte> body)
    {
        var reader = new Reader(body);
        _ = reader.U8();
        _ = reader.U8();
        _ = reader.Bytes(reader.U16());
        _ = reader.Text8();
        _ = reader.U64();
        _ = reader.Text8();
        _ = reader.Text8();
        _ = reader.U64();
        var start = reader.Offset;
        if (reader.U8() > 1)
            throw new InvalidDataException(
                "The authority relocation presence flag is invalid.");
        _ = reader.Bytes(reader.U32AsInt());
        return (start, reader.Offset);
    }

    private static byte[] EncodeSlot(
        ZLinkCanonicalRelocationAuthorityState value,
        ZLinkRelocationEnvelope? root)
    {
        if (value.Phase is < 1 or > 9 || value.SourceCleanupState > 2
            || value.ApplicationVersion < 0)
            throw new ArgumentOutOfRangeException(nameof(value));
        foreach (var fence in new[]
                 {
                     value.SourceNodeGeneration,
                     value.SourceOwnerLeaseGeneration,
                     value.CoordinatorLeaseGeneration,
                     value.CoordinatorNodeGeneration
                 })
            if (fence == 0)
                throw new ArgumentOutOfRangeException(nameof(value));
        var sourceOnly = value.Phase is 1 or 2;
        if (sourceOnly
            && (value.TargetAttemptGeneration != 0
                || value.TargetNodeGeneration != 0
                || value.TargetOwnerLeaseGeneration != 0
                || value.ReservationGeneration != 0
                || value.TargetNodeRid.Length != 0
                || value.TargetOwnerId.Length != 0)
            || !sourceOnly
            && value.Phase != 9
            && (value.TargetAttemptGeneration == 0
                || value.TargetNodeGeneration == 0
                || value.TargetOwnerLeaseGeneration == 0
                || value.ReservationGeneration == 0
                || value.TargetNodeRid.Length == 0
                || value.TargetOwnerId.Length == 0))
            throw new ArgumentException(
                "Canonical relocation target fields do not match the phase.",
                nameof(value));
        if (value.Phase == 1
            && (root is not null
                || value.RelocationReference.Length != 0
                || value.RelocationChecksumCrc32c != 0)
            || value.Phase != 1
            && (root is null
                || value.RelocationReference.Length == 0))
            throw new ArgumentException(
                "Canonical relocation root does not match the phase.",
                nameof(value));
        using var body = new MemoryStream();
        WriteU64(body, value.RelocationHigh);
        WriteU64(body, value.RelocationLow);
        WriteU64(body, value.TargetAttemptGeneration);
        WriteText8(body, value.SourceNodeRid, optional: false);
        WriteU64(body, value.SourceNodeGeneration);
        WriteText8(body, value.SourceOwnerId, optional: false);
        WriteU64(body, value.SourceOwnerLeaseGeneration);
        WriteText8(body, value.TargetNodeRid, optional: true);
        WriteU64(body, value.TargetNodeGeneration);
        WriteText8(body, value.TargetOwnerId, optional: true);
        WriteU64(body, value.TargetOwnerLeaseGeneration);
        WriteU64(body, value.ReservationGeneration);
        WriteText8(body, value.CoordinatorOwnerId, optional: false);
        WriteU64(body, value.CoordinatorLeaseGeneration);
        WriteText8(body, value.CoordinatorNodeRid, optional: false);
        WriteU64(body, value.CoordinatorNodeGeneration);
        body.WriteByte(value.Phase);
        if (root is null)
        {
            body.WriteByte(0);
            WriteU16(body, 0);
        }
        else
        {
            using var pointer = new MemoryStream();
            WriteText16(pointer, value.RelocationReference);
            WriteU32(pointer, value.RelocationChecksumCrc32c);
            body.WriteByte(1);
            WriteU16(body, checked((ushort)pointer.Length));
            pointer.Position = 0;
            pointer.CopyTo(body);
        }
        WriteI64(body, value.ApplicationVersion);
        WriteU32(body, checked((uint)(root?.Participants.Count ?? 0)));
        foreach (var participant in (root?.Participants
                     ?? Array.Empty<ZLinkRelocationParticipantEnvelope>()).OrderBy(
                     static participant => participant.CanonicalParticipantId))
        {
            if (participant.CanonicalParticipantId == 0)
                throw new ArgumentException(
                    "Canonical root participant progress is unavailable.", nameof(root));
            WriteU64(body, participant.CanonicalParticipantId);
            WriteU64(body, participant.AcceptedBoundary);
            WriteU64(body, participant.ReplayCursor);
        }
        WriteU32(body, checked((uint)(root?.Participants.Sum(
            static participant => participant.TerminalCompletions.Count) ?? 0)));
        WriteU32(body, checked((uint)(root?.Participants.Sum(
            static participant => participant.PendingRelayCount) ?? 0)));
        body.WriteByte(value.SourceCleanupState);
        body.WriteByte(1);
        WriteU64(
            body,
            root?.AggregateGeneration ?? value.AggregateGeneration);
        using var slot = new MemoryStream();
        slot.WriteByte(1);
        WriteU32(slot, checked((uint)body.Length));
        body.Position = 0;
        body.CopyTo(slot);
        return slot.ToArray();
    }

    private static void WriteText8(Stream stream, string value, bool optional)
    {
        var bytes = StrictUtf8.GetBytes(value);
        if (optional && bytes.Length == 0)
        {
            stream.WriteByte(0);
            return;
        }
        if (bytes.Length is < 1 or > byte.MaxValue || bytes.AsSpan().Contains((byte)0))
            throw new ArgumentOutOfRangeException(nameof(value));
        stream.WriteByte(checked((byte)bytes.Length));
        stream.Write(bytes);
    }

    private static void WriteText16(Stream stream, string value)
    {
        var bytes = StrictUtf8.GetBytes(value);
        if (bytes.Length is < 1 or > 4096 || bytes.AsSpan().Contains((byte)0))
            throw new ArgumentOutOfRangeException(nameof(value));
        WriteU16(stream, checked((ushort)bytes.Length));
        stream.Write(bytes);
    }

    private static void WriteU16(Stream stream, ushort value)
    {
        Span<byte> bytes = stackalloc byte[2];
        BinaryPrimitives.WriteUInt16BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private static void WriteU32(Stream stream, uint value)
    {
        Span<byte> bytes = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private static void WriteU64(Stream stream, ulong value)
    {
        Span<byte> bytes = stackalloc byte[8];
        BinaryPrimitives.WriteUInt64BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private static void WriteI64(Stream stream, long value)
    {
        Span<byte> bytes = stackalloc byte[8];
        BinaryPrimitives.WriteInt64BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private ref struct Reader(ReadOnlySpan<byte> source)
    {
        private readonly ReadOnlySpan<byte> _source = source;
        private int _offset;
        internal int Offset => _offset;
        internal bool End => _offset == _source.Length;
        internal byte U8() => Bytes(1)[0];
        internal ushort U16() => BinaryPrimitives.ReadUInt16BigEndian(Bytes(2));
        internal uint U32() => BinaryPrimitives.ReadUInt32BigEndian(Bytes(4));
        internal ulong U64() => BinaryPrimitives.ReadUInt64BigEndian(Bytes(8));
        internal long I64() => BinaryPrimitives.ReadInt64BigEndian(Bytes(8));
        internal int U32AsInt() => checked((int)U32());
        internal ReadOnlySpan<byte> Bytes(int count)
        {
            if (count < 0 || count > _source.Length - _offset)
                throw new EndOfStreamException();
            var result = _source.Slice(_offset, count);
            _offset += count;
            return result;
        }
        internal string Text8()
        {
            var bytes = Bytes(U8());
            if (bytes.IsEmpty || bytes.Contains((byte)0))
                throw new InvalidDataException();
            return StrictUtf8.GetString(bytes);
        }
        internal string OptionalText8()
        {
            var length = U8();
            return length == 0 ? string.Empty : StrictUtf8.GetString(Bytes(length));
        }
        internal string Text16()
        {
            var bytes = Bytes(U16());
            if (bytes.IsEmpty || bytes.Contains((byte)0))
                throw new InvalidDataException();
            return StrictUtf8.GetString(bytes);
        }
    }
}
