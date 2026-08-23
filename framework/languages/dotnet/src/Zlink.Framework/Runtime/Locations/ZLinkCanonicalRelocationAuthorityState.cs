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
    string CoordinatorOwnerId,
    ulong CoordinatorLeaseGeneration,
    string CoordinatorNodeRid,
    ulong CoordinatorNodeGeneration,
    byte Phase,
    long ApplicationVersion)
{
    internal ZLinkCanonicalRelocationAuthorityState(
        ulong relocationHigh,
        ulong relocationLow,
        ulong targetAttemptGeneration,
        string sourceNodeRid,
        ulong sourceNodeGeneration,
        string sourceOwnerId,
        ulong sourceOwnerLeaseGeneration,
        string targetNodeRid,
        ulong targetNodeGeneration,
        string targetOwnerId,
        ulong targetOwnerLeaseGeneration,
        string coordinatorOwnerId,
        ulong coordinatorLeaseGeneration,
        string coordinatorNodeRid,
        ulong coordinatorNodeGeneration,
        byte phase,
        string relocationReference,
        uint relocationChecksumCrc32c,
        long applicationVersion)
        : this(
            relocationHigh,
            relocationLow,
            targetAttemptGeneration,
            sourceNodeRid,
            sourceNodeGeneration,
            sourceOwnerId,
            sourceOwnerLeaseGeneration,
            targetNodeRid,
            targetNodeGeneration,
            targetOwnerId,
            targetOwnerLeaseGeneration,
            coordinatorOwnerId,
            coordinatorLeaseGeneration,
            coordinatorNodeRid,
            coordinatorNodeGeneration,
            phase,
            applicationVersion)
    {
        RelocationReference = relocationReference;
        RelocationChecksumCrc32c = relocationChecksumCrc32c;
    }

    internal ulong AggregateGeneration { get; init; }
    internal string CoordinatorExpectedAuthorityStoreVersion { get; init; } = "";
    internal string RelocationReference { get; init; } = "";
    internal uint RelocationChecksumCrc32c { get; init; }
    internal byte SourceCleanupState { get; init; }
}

internal sealed record ZLinkCanonicalRelocationAuthorityProjection(
    ulong RelocationHigh,
    ulong RelocationLow,
    ulong TargetAttemptGeneration,
    string TargetOwnerId,
    ulong TargetOwnerLeaseGeneration,
    byte Phase,
    long ApplicationVersion,
    string RelocationReference,
    uint RelocationChecksumCrc32c,
    ReadOnlyMemory<byte> SteadyAuthorityPayload,
    ZLinkCanonicalRelocationAuthorityState State)
{
    internal ZLinkCanonicalRelocationAuthorityProjection(
        ulong relocationHigh,
        ulong relocationLow,
        ulong targetAttemptGeneration,
        string targetOwnerId,
        ulong targetOwnerLeaseGeneration,
        byte phase,
        string relocationReference,
        uint relocationChecksumCrc32c,
        long applicationVersion,
        ReadOnlyMemory<byte> steadyAuthorityPayload,
        ZLinkCanonicalRelocationAuthorityState state)
        : this(
            relocationHigh,
            relocationLow,
            targetAttemptGeneration,
            targetOwnerId,
            targetOwnerLeaseGeneration,
            phase,
            applicationVersion,
            relocationReference,
            relocationChecksumCrc32c,
            steadyAuthorityPayload,
            state)
    {
    }

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
            var presence = bodyReader.U8();
            var slotBody = bodyReader.Bytes(bodyReader.U32AsInt());
            if (presence != 1
                || !TryReadSlotBody(
                    slotBody,
                    expectedRootAggregateGeneration: null,
                    validateRootAgreement: false,
                    out var state))
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
            projection = new ZLinkCanonicalRelocationAuthorityProjection(
                state.RelocationHigh,
                state.RelocationLow,
                state.TargetAttemptGeneration,
                state.TargetOwnerId,
                state.TargetOwnerLeaseGeneration,
                state.Phase,
                state.ApplicationVersion,
                state.RelocationReference,
                state.RelocationChecksumCrc32c,
                steady.ToArray(), state)
            {
                AggregateGeneration = state.AggregateGeneration
            };
            return true;
        }
        catch (Exception error) when (error is IOException
                                      or InvalidDataException
                                      or OverflowException
                                      or DecoderFallbackException
                                      or ArgumentException)
        {
            return false;
        }
    }

    internal static bool TryReadSlot(
        ReadOnlySpan<byte> slot,
        ulong? expectedRootAggregateGeneration,
        out ZLinkCanonicalRelocationAuthorityState state)
    {
        state = null!;
        try
        {
            var reader = new Reader(slot);
            if (reader.U8() != 1)
                return false;
            var body = reader.Bytes(reader.U32AsInt());
            return reader.End
                   && TryReadSlotBody(
                       body,
                       expectedRootAggregateGeneration,
                       validateRootAgreement: true,
                       out state);
        }
        catch (Exception error) when (error is IOException
                                      or InvalidDataException
                                      or OverflowException
                                      or DecoderFallbackException
                                      or ArgumentException)
        {
            return false;
        }
    }

    private static bool TryReadSlotBody(
        ReadOnlySpan<byte> body,
        ulong? expectedRootAggregateGeneration,
        bool validateRootAgreement,
        out ZLinkCanonicalRelocationAuthorityState state)
    {
        state = null!;
        var relocation = new Reader(body);
        var high = relocation.U64();
        var low = relocation.U64();
        var aggregateGeneration = relocation.U64();
        var attempt = relocation.U64();
        var reference = relocation.Text16();
        var checksum = relocation.U32();
        var sourceNodeRid = relocation.Rid8(required: true);
        var sourceNodeGeneration = relocation.U64();
        var sourceOwner = relocation.Text8();
        var sourceOwnerLease = relocation.U64();
        var targetNodeRid = relocation.Rid8(required: false);
        var targetNodeGeneration = relocation.U64();
        var targetOwner = relocation.OptionalText8();
        var targetLease = relocation.U64();
        var coordinatorOwner = relocation.Text8();
        var coordinatorLease = relocation.U64();
        var coordinatorNodeRid = relocation.Rid8(required: true);
        var coordinatorNodeGeneration = relocation.U64();
        var coordinatorExpectedStoreVersion = relocation.OptionalText8();
        var phase = relocation.U8();
        var applicationVersion = relocation.I64();
        var sourceCleanupState = relocation.U8();
        if (!relocation.End)
            return false;

        var decoded = new ZLinkCanonicalRelocationAuthorityState(
            high,
            low,
            attempt,
            sourceNodeRid,
            sourceNodeGeneration,
            sourceOwner,
            sourceOwnerLease,
            targetNodeRid,
            targetNodeGeneration,
            targetOwner,
            targetLease,
            coordinatorOwner,
            coordinatorLease,
            coordinatorNodeRid,
            coordinatorNodeGeneration,
            phase,
            applicationVersion)
        {
            AggregateGeneration = aggregateGeneration,
            CoordinatorExpectedAuthorityStoreVersion =
                coordinatorExpectedStoreVersion,
            RelocationReference = reference,
            RelocationChecksumCrc32c = checksum,
            SourceCleanupState = sourceCleanupState
        };
        if (!IsValidState(
                decoded,
                expectedRootAggregateGeneration,
                validateRootAgreement))
            return false;
        state = decoded;
        return true;
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

        var slot = EncodeSlot(state, root?.AggregateGeneration);
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

    internal static byte[] EncodeSlot(
        ZLinkCanonicalRelocationAuthorityState value,
        ulong? expectedRootAggregateGeneration)
    {
        if (!IsValidState(
                value,
                expectedRootAggregateGeneration,
                validateRootAgreement: true))
            throw new ArgumentException(
                "Canonical relocation state does not match the closed authority slot contract.",
                nameof(value));
        using var body = new MemoryStream();
        WriteU64(body, value.RelocationHigh);
        WriteU64(body, value.RelocationLow);
        WriteU64(body, value.AggregateGeneration);
        WriteU64(body, value.TargetAttemptGeneration);
        WriteText16(body, value.RelocationReference);
        WriteU32(body, value.RelocationChecksumCrc32c);
        WriteRid8(body, value.SourceNodeRid, optional: false);
        WriteU64(body, value.SourceNodeGeneration);
        WriteText8(body, value.SourceOwnerId, optional: false);
        WriteU64(body, value.SourceOwnerLeaseGeneration);
        WriteRid8(body, value.TargetNodeRid, optional: true);
        WriteU64(body, value.TargetNodeGeneration);
        WriteText8(body, value.TargetOwnerId, optional: true);
        WriteU64(body, value.TargetOwnerLeaseGeneration);
        WriteText8(body, value.CoordinatorOwnerId, optional: false);
        WriteU64(body, value.CoordinatorLeaseGeneration);
        WriteRid8(body, value.CoordinatorNodeRid, optional: false);
        WriteU64(body, value.CoordinatorNodeGeneration);
        WriteText8(
            body,
            value.CoordinatorExpectedAuthorityStoreVersion,
            optional: true);
        body.WriteByte(value.Phase);
        WriteI64(body, value.ApplicationVersion);
        body.WriteByte(value.SourceCleanupState);
        using var slot = new MemoryStream();
        slot.WriteByte(1);
        WriteU32(slot, checked((uint)body.Length));
        body.Position = 0;
        body.CopyTo(slot);
        return slot.ToArray();
    }

    private static bool IsValidState(
        ZLinkCanonicalRelocationAuthorityState value,
        ulong? expectedRootAggregateGeneration,
        bool validateRootAgreement)
    {
        const ulong maximumOrdinal = long.MaxValue;
        const ulong maximumIssuedAggregateGeneration = long.MaxValue - 1UL;
        if (value.RelocationHigh == 0 && value.RelocationLow == 0
            || value.AggregateGeneration > maximumIssuedAggregateGeneration
            || value.TargetAttemptGeneration > maximumOrdinal
            || value.SourceNodeGeneration is 0 or > maximumOrdinal
            || value.SourceOwnerLeaseGeneration is 0 or > maximumOrdinal
            || value.TargetNodeGeneration > maximumOrdinal
            || value.TargetOwnerLeaseGeneration > maximumOrdinal
            || value.CoordinatorLeaseGeneration is 0 or > maximumOrdinal
            || value.CoordinatorNodeGeneration is 0 or > maximumOrdinal
            || value.Phase is < 1 or > 9
            || value.ApplicationVersion < 0
            || value.SourceCleanupState > 2
            || string.IsNullOrEmpty(value.RelocationReference)
            || string.IsNullOrEmpty(value.SourceNodeRid)
            || string.IsNullOrEmpty(value.SourceOwnerId)
            || string.IsNullOrEmpty(value.CoordinatorOwnerId)
            || string.IsNullOrEmpty(value.CoordinatorNodeRid))
            return false;

        if (value.Phase == 1)
        {
            if (value.AggregateGeneration != 0
                || validateRootAgreement
                && expectedRootAggregateGeneration is not null)
                return false;
        }
        else if (value.AggregateGeneration == 0
                 || validateRootAgreement
                 && expectedRootAggregateGeneration != value.AggregateGeneration)
        {
            return false;
        }

        var targetEmpty = value.TargetAttemptGeneration == 0
                          && value.TargetNodeRid.Length == 0
                          && value.TargetNodeGeneration == 0
                          && value.TargetOwnerId.Length == 0
                          && value.TargetOwnerLeaseGeneration == 0;
        var targetComplete = value.TargetAttemptGeneration != 0
                             && value.TargetNodeRid.Length != 0
                             && value.TargetNodeGeneration != 0
                             && value.TargetOwnerId.Length != 0
                             && value.TargetOwnerLeaseGeneration != 0;
        return value.Phase is 1 or 2 ? targetEmpty : targetComplete;
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

    private static void WriteRid8(Stream stream, string value, bool optional)
    {
        if (optional && value.Length == 0)
        {
            stream.WriteByte(0);
            return;
        }
        byte[] bytes;
        try
        {
            bytes = Convert.FromHexString(value);
        }
        catch (FormatException error)
        {
            throw new ArgumentOutOfRangeException(nameof(value), error);
        }
        if (bytes.Length is < 1 or > byte.MaxValue)
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
            if (length == 0)
                return string.Empty;
            var bytes = Bytes(length);
            if (bytes.Contains((byte)0))
                throw new InvalidDataException();
            return StrictUtf8.GetString(bytes);
        }
        internal string Text16()
        {
            var bytes = Bytes(U16());
            if (bytes.IsEmpty || bytes.Contains((byte)0))
                throw new InvalidDataException();
            return StrictUtf8.GetString(bytes);
        }
        internal string Rid8(bool required)
        {
            var bytes = Bytes(U8());
            if (required && bytes.IsEmpty)
                throw new InvalidDataException();
            return Convert.ToHexString(bytes).ToLowerInvariant();
        }
    }
}
