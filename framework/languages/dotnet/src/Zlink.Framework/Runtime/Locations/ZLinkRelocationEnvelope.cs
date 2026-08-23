using System.Buffers;
using System.Buffers.Binary;
using System.Text;
using System.Security.Cryptography;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Locations;

internal sealed record ZLinkRelocationQueuedJob(
    ulong AcceptedSequence,
    ReadOnlyMemory<byte> Payload)
{
    internal ZLinkCanonicalRequestSourceFence? RequestSource { get; init; }
    internal ZLinkCanonicalAcceptedRequest? CanonicalRequest { get; init; }
}

internal sealed record ZLinkCanonicalRequestSourceFence(
    string OwnerId,
    ulong OwnerLeaseGeneration,
    string NodeRid,
    ulong NodeGeneration);

internal sealed record ZLinkCanonicalAcceptedRequest(
    ZLinkCanonicalRequestSourceFence Source,
    string? SourceSpotId,
    ulong OperationHigh,
    ulong OperationLow,
    ulong ReplyRouteId,
    string TargetSpotId,
    ulong TargetSpotGeneration,
    string TargetNodeRid,
    ulong TargetNodeGeneration,
    ulong TargetAuthorityOwnerGeneration,
    ulong TargetOwnerLeaseGeneration,
    ZLinkMessageMetadata Metadata,
    ZLinkCanonicalApplicationPayload ApplicationPayload);

internal sealed record ZLinkRelocationLogicalTimer(
    string TimerId,
    long DueUnixTimeMilliseconds,
    long PeriodMilliseconds,
    ReadOnlyMemory<byte> Payload)
{
    internal ulong PendingAcceptedSequence { get; init; }
    internal ZLinkCanonicalLogicalTimer? CanonicalTimer { get; init; }
}

internal sealed record ZLinkCanonicalLogicalTimer(
    string HandlerType,
    byte OverrunPolicy,
    ulong MaxCatchUpTicks,
    bool StopOnUnhandledException,
    ulong LastCompletedDeliveryIndex,
    ulong LastCompletedScheduledIndex,
    long NextScheduledAtUnixMilliseconds,
    ZLinkCanonicalPendingTimerTick? PendingTick);

internal sealed record ZLinkCanonicalPendingTimerTick(
    ulong AcceptedSequence,
    ulong DeliveryIndex,
    ulong ScheduledIndex,
    long ScheduledAtUnixMilliseconds,
    ulong SkippedTicks);

internal sealed record ZLinkCanonicalTerminalCompletion(
    ulong OperationHigh,
    ulong OperationLow,
    string SourceOwnerId,
    ulong SourceOwnerLeaseGeneration,
    string SourceNodeRid,
    ulong SourceNodeGeneration,
    ulong ParticipantId,
    ulong AcceptedSequence,
    uint TerminalResult,
    uint ErrorCode)
{
    internal ZLinkCanonicalApplicationPayload? Payload { get; init; }
}

internal sealed record ZLinkCanonicalApplicationPayload(
    string PacketName,
    string ContentType,
    ReadOnlyMemory<byte> Payload);

internal sealed record ZLinkRelocationParticipantEnvelope(
    ZLinkAuthorityKey AuthorityKey,
    ZLinkPlacementObjectKind ObjectKind,
    ulong ObjectGeneration,
    ulong AuthorityOwnerGeneration,
    ReadOnlyMemory<byte> ApplicationState,
    IReadOnlyList<ZLinkRelocationQueuedJob> AcceptedJobs,
    IReadOnlyList<ZLinkRelocationLogicalTimer> LogicalTimers,
    ReadOnlyMemory<byte> RecoveryPayload = default,
    ReadOnlyMemory<byte> CompletionPayload = default)
{
    internal ulong CanonicalParticipantId { get; init; }
}

internal sealed record ZLinkRelocationEnvelope(
    Guid AggregateId,
    ulong AggregateGeneration,
    ReadOnlyMemory<byte> InventoryDigest,
    IReadOnlyList<ZLinkRelocationParticipantEnvelope> Participants)
{
    // A decoded protocol-schema logical stream is retained byte-for-byte.  The
    // current runtime projection does not expose every frozen-record field yet,
    // so re-encoding the projection would silently discard durable information.
    internal ReadOnlyMemory<byte> CanonicalLogicalStream { get; init; }
    internal ulong CanonicalRelocationHigh { get; init; }
    internal ulong CanonicalRelocationLow { get; init; }
    internal long CanonicalApplicationVersion { get; init; }
}

internal static class ZLinkRelocationEnvelopeCodec
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    private const uint Magic = 0x5a4c5231; // ZLR1
    private const ushort Version = 2;
    private const int MaxFieldBytes = 64 * 1024 * 1024;
    private const int MinEncodedParticipantBytes = 38;
    private const int MinEncodedAcceptedJobBytes = sizeof(ulong) + sizeof(int);
    private const int MaxTimerItems = 65_536;

    internal static byte[] Encode(ZLinkRelocationEnvelope envelope)
    {
        using var stream = new MemoryStream();
        EncodeTo(stream, envelope);
        return stream.ToArray();
    }

    internal static bool TryValidateCanonicalFrozenRecord(ReadOnlySpan<byte> encoded)
        => TryValidateCanonicalFrozenRecord(encoded, out _);

    internal static bool TryValidateCanonicalFrozenRecord(
        ReadOnlySpan<byte> encoded, out bool truncated)
    {
        truncated = false;
        try
        {
            var reader = new CanonicalReader(encoded.ToArray());
            ReadCanonicalFrozenRecord(ref reader);
            reader.RequireEnd("relocation frozen record");
            return true;
        }
        catch (InvalidDataException)
        {
            return false;
        }
        catch (EndOfStreamException)
        {
            truncated = true;
            return false;
        }
    }

    internal static long MeasureEncodedLength(ZLinkRelocationEnvelope envelope)
    {
        using var stream = new CountingStream();
        EncodeTo(stream, envelope);
        return stream.Length;
    }

    internal static byte[] ComputeEncodedSha256(ZLinkRelocationEnvelope envelope)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        using var stream = new HashStream(hash);
        EncodeTo(stream, envelope);
        return hash.GetHashAndReset();
    }


    internal static ZLinkCanonicalTerminalCompletion CreateCanonicalTerminalCompletion(
        ulong operationHigh,
        ulong operationLow,
        string sourceOwnerId,
        ulong sourceOwnerLeaseGeneration,
        string sourceNodeRid,
        ulong sourceNodeGeneration,
        ulong participantId,
        ulong acceptedSequence,
        uint terminalResult,
        uint errorCode,
        ZLinkCanonicalApplicationPayload? payload)
    {
        if (operationHigh == 0 && operationLow == 0
            || sourceOwnerLeaseGeneration == 0
            || sourceNodeGeneration == 0
            || participantId == 0
            || acceptedSequence == 0
            || !IsCanonicalTerminalResult(terminalResult)
            //  Schema terminal-failure-integrity: the completion's
            //  terminal/failure pair must match the schema table, and a
            //  failure terminal carries no application payload.
            || !ServiceWireConstants.ValidTerminalFailure(
                terminalResult, errorCode)
            || terminalResult != 0 && payload is not null
            || !IsText8(sourceOwnerId)
            || !IsText8(sourceNodeRid)
            || payload is not null
               && (!IsText8(payload.PacketName)
                   || !IsText8(payload.ContentType)
                   || payload.Payload.Length > MaxFieldBytes))
            throw new ArgumentOutOfRangeException(nameof(operationLow));
        return new ZLinkCanonicalTerminalCompletion(
            operationHigh,
            operationLow,
            sourceOwnerId,
            sourceOwnerLeaseGeneration,
            sourceNodeRid,
            sourceNodeGeneration,
            participantId,
            acceptedSequence,
            terminalResult,
            errorCode)
        {
            Payload = payload
        };

        static bool IsText8(string value)
        {
            var encoded = StrictUtf8.GetBytes(value);
            return encoded.Length is > 0 and <= byte.MaxValue
                   && !encoded.AsSpan().Contains((byte)0);
        }
    }

    internal static void EncodeTo(
        Stream stream,
        ZLinkRelocationEnvelope envelope)
    {
        ArgumentNullException.ThrowIfNull(stream);
        ArgumentNullException.ThrowIfNull(envelope);
        if (!envelope.CanonicalLogicalStream.IsEmpty)
        {
            stream.Write(EncodeCanonical(envelope.CanonicalLogicalStream));
            return;
        }
        ValidateEnvelope(envelope);
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        writer.Write(Magic);
        writer.Write(Version);
        writer.Write(envelope.AggregateId.ToByteArray());
        writer.Write(envelope.AggregateGeneration);
        WriteBytes(writer, envelope.InventoryDigest.Span);
        writer.Write(envelope.Participants.Count);
        foreach (var participant in envelope.Participants)
        {
            WriteString(writer, participant.AuthorityKey.Value);
            writer.Write((byte)participant.ObjectKind);
            writer.Write(participant.ObjectGeneration);
            writer.Write(participant.AuthorityOwnerGeneration);
            WriteBytes(writer, participant.ApplicationState.Span);
            writer.Write(participant.AcceptedJobs.Count);
            foreach (var job in participant.AcceptedJobs)
            {
                writer.Write(job.AcceptedSequence);
                WriteBytes(writer, job.Payload.Span);
            }
            writer.Write(participant.LogicalTimers.Count);
            foreach (var timer in participant.LogicalTimers)
            {
                WriteString(writer, timer.TimerId);
                writer.Write(timer.DueUnixTimeMilliseconds);
                writer.Write(timer.PeriodMilliseconds);
                WriteBytes(writer, timer.Payload.Span);
            }
            WriteBytes(writer, participant.RecoveryPayload.Span);
            WriteBytes(writer, participant.CompletionPayload.Span);
        }
        writer.Flush();
    }

    internal static ZLinkRelocationEnvelope Decode(ReadOnlySpan<byte> encoded)
    {
        if (encoded.Length < sizeof(uint))
            throw new InvalidDataException(
                "The relocation logical stream is truncated.");
        var bytes = encoded.ToArray();
        if (BinaryPrimitives.ReadUInt32LittleEndian(encoded) != Magic)
            return DecodeCanonical(bytes, default);
        return DecodeVersioned(bytes);
    }

    internal static ZLinkRelocationEnvelope Decode(
        Stream stream,
        ReadOnlyMemory<byte> inventoryDigest = default)
    {
        ArgumentNullException.ThrowIfNull(stream);
        if (!stream.CanSeek)
        {
            var memory = ReadNonSeekableOwnedMemory(stream);
            if (memory.Length < sizeof(uint))
                throw new InvalidDataException(
                    "The relocation logical stream is truncated.");
            if (BinaryPrimitives.ReadUInt32LittleEndian(memory.Span)
                != Magic)
                return DecodeCanonical(memory, inventoryDigest);
            return DecodeVersioned(memory);
        }

        var start = stream.Position;
        Span<byte> header = stackalloc byte[sizeof(uint)];
        try
        {
            stream.ReadExactly(header);
        }
        catch (EndOfStreamException)
        {
            throw new InvalidDataException("The relocation logical stream is truncated.");
        }
        finally
        {
            stream.Position = start;
        }
        if (BinaryPrimitives.ReadUInt32LittleEndian(header) != Magic)
        {
            var remaining = stream.Length - start;
            if (remaining > int.MaxValue)
                throw new InvalidDataException(
                    "The canonical relocation logical stream exceeds its decode bound.");
            var bytes = new byte[checked((int)remaining)];
            stream.ReadExactly(bytes);
            return DecodeCanonical(bytes, inventoryDigest);
        }
        return DecodeVersioned(stream);
    }

    private static ReadOnlyMemory<byte> ReadNonSeekableOwnedMemory(
        Stream stream)
    {
        const int segmentBytes = 1024 * 1024;
        var segments = new List<(byte[] Buffer, int Count)>();
        long total = 0;
        try
        {
            while (true)
            {
                var buffer = ArrayPool<byte>.Shared.Rent(segmentBytes);
                var count = 0;
                while (count < buffer.Length)
                {
                    var read = stream.Read(
                        buffer,
                        count,
                        buffer.Length - count);
                    if (read == 0) break;
                    count += read;
                }
                if (count == 0)
                {
                    ArrayPool<byte>.Shared.Return(buffer);
                    break;
                }
                total = checked(total + count);
                if (total > int.MaxValue)
                    throw new InvalidDataException(
                        "The relocation logical stream exceeds its decode bound.");
                segments.Add((buffer, count));
                if (count < buffer.Length) break;
            }

            var owned = GC.AllocateUninitializedArray<byte>(
                checked((int)total));
            var offset = 0;
            foreach (var segment in segments)
            {
                segment.Buffer.AsSpan(0, segment.Count)
                    .CopyTo(owned.AsSpan(offset));
                offset += segment.Count;
            }
            return owned;
        }
        finally
        {
            foreach (var segment in segments)
                ArrayPool<byte>.Shared.Return(segment.Buffer);
        }
    }

    private static ZLinkRelocationEnvelope DecodeVersioned(
        ReadOnlyMemory<byte> encoded)
    {
        var reader = new VersionedMemoryReader(encoded);
        if (reader.ReadUInt32() != Magic || reader.ReadUInt16() != Version)
            throw new InvalidDataException(
                "The relocation root header is invalid.");
        var aggregateId = new Guid(reader.ReadBytes(16).Span);
        var aggregateGeneration = reader.ReadUInt64();
        var decodedInventoryDigest = reader.ReadByteField(32);
        if (decodedInventoryDigest.Length != 32)
            throw new InvalidDataException(
                "The relocation inventory digest must contain 32 bytes.");
        var participantCount = reader.ReadCount(
            int.MaxValue,
            "participant");
        if (participantCount
            > reader.Remaining / MinEncodedParticipantBytes)
            throw new InvalidDataException(
                "The relocation participant count exceeds the remaining payload.");
        var participants =
            new ZLinkRelocationParticipantEnvelope[participantCount];
        for (var participantIndex = 0;
             participantIndex < participantCount;
             participantIndex++)
        {
            var key = new ZLinkAuthorityKey(reader.ReadString());
            var objectKind =
                (ZLinkPlacementObjectKind)reader.ReadByte();
            if (!Enum.IsDefined(objectKind))
                throw new InvalidDataException(
                    "The relocation object kind is invalid.");
            var objectGeneration = reader.ReadUInt64();
            var ownerGeneration = reader.ReadUInt64();
            if (objectGeneration == 0 || ownerGeneration == 0)
                throw new InvalidDataException(
                    "Relocation participant generations must be non-zero.");
            var state = reader.ReadByteField(MaxFieldBytes);
            var jobs = new ZLinkRelocationQueuedJob[
                reader.ReadCountWithinRemaining(
                    MinEncodedAcceptedJobBytes,
                    "accepted job")];
            ulong previousSequence = 0;
            for (var jobIndex = 0;
                 jobIndex < jobs.Length;
                 jobIndex++)
            {
                var sequence = reader.ReadUInt64();
                if (sequence == 0 || sequence <= previousSequence)
                    throw new InvalidDataException(
                        "Accepted job sequences must be strictly increasing.");
                previousSequence = sequence;
                jobs[jobIndex] = new ZLinkRelocationQueuedJob(
                    sequence,
                    reader.ReadByteField(MaxFieldBytes));
            }
            var timers = new ZLinkRelocationLogicalTimer[
                reader.ReadCount(
                    MaxTimerItems,
                    "logical timer")];
            var timerIds = new HashSet<string>(StringComparer.Ordinal);
            for (var timerIndex = 0;
                 timerIndex < timers.Length;
                 timerIndex++)
            {
                var timerId = reader.ReadString();
                if (!timerIds.Add(timerId))
                    throw new InvalidDataException(
                        $"Duplicate logical timer '{timerId}'.");
                var due = reader.ReadInt64();
                var period = reader.ReadInt64();
                if (period < 0)
                    throw new InvalidDataException(
                        "A logical timer period cannot be negative.");
                timers[timerIndex] = new ZLinkRelocationLogicalTimer(
                    timerId,
                    due,
                    period,
                    reader.ReadByteField(MaxFieldBytes));
            }
            participants[participantIndex] =
                new ZLinkRelocationParticipantEnvelope(
                    key,
                    objectKind,
                    objectGeneration,
                    ownerGeneration,
                    state,
                    jobs,
                    timers,
                    reader.ReadByteField(MaxFieldBytes),
                    reader.ReadByteField(MaxFieldBytes));
        }
        reader.RequireEnd("relocation root");
        var envelope = HydrateCanonicalParticipantIdentity(
            new ZLinkRelocationEnvelope(
            aggregateId,
            aggregateGeneration,
            decodedInventoryDigest,
            participants));
        ValidateEnvelope(envelope);
        return envelope;
    }

    private static ZLinkRelocationEnvelope DecodeVersioned(Stream input)
    {
        using var reader = new BinaryReader(input, Encoding.UTF8, leaveOpen: true);
        if (reader.ReadUInt32() != Magic || reader.ReadUInt16() != Version)
            throw new InvalidDataException("The relocation root header is invalid.");
        var aggregateId = new Guid(ReadExact(reader, 16));
        var aggregateGeneration = reader.ReadUInt64();
        var decodedInventoryDigest = ReadBytes(reader, 32);
        if (decodedInventoryDigest.Length != 32)
            throw new InvalidDataException(
                "The relocation inventory digest must contain 32 bytes.");
        var participantCount = ReadCount(reader, int.MaxValue, "participant");
        if (participantCount
            > (input.Length - input.Position) / MinEncodedParticipantBytes)
            throw new InvalidDataException(
                "The relocation participant count exceeds the remaining payload.");
        var participants = new ZLinkRelocationParticipantEnvelope[participantCount];
        for (var participantIndex = 0; participantIndex < participantCount; participantIndex++)
        {
            var key = new ZLinkAuthorityKey(ReadString(reader));
            var objectKind = (ZLinkPlacementObjectKind)reader.ReadByte();
            if (!Enum.IsDefined(objectKind))
                throw new InvalidDataException("The relocation object kind is invalid.");
            var objectGeneration = reader.ReadUInt64();
            var ownerGeneration = reader.ReadUInt64();
            if (objectGeneration == 0 || ownerGeneration == 0)
                throw new InvalidDataException(
                    "Relocation participant generations must be non-zero.");
            var state = ReadBytes(reader, MaxFieldBytes);
            var jobs = new ZLinkRelocationQueuedJob[
                ReadCountWithinRemaining(
                    reader,
                    input,
                    MinEncodedAcceptedJobBytes,
                    "accepted job")];
            ulong previousSequence = 0;
            for (var jobIndex = 0; jobIndex < jobs.Length; jobIndex++)
            {
                var sequence = reader.ReadUInt64();
                if (sequence == 0 || sequence <= previousSequence)
                    throw new InvalidDataException(
                        "Accepted job sequences must be strictly increasing.");
                previousSequence = sequence;
                jobs[jobIndex] = new ZLinkRelocationQueuedJob(
                    sequence,
                    ReadBytes(reader, MaxFieldBytes));
            }
            var timers = new ZLinkRelocationLogicalTimer[
                ReadCount(reader, MaxTimerItems, "logical timer")];
            var timerIds = new HashSet<string>(StringComparer.Ordinal);
            for (var timerIndex = 0; timerIndex < timers.Length; timerIndex++)
            {
                var timerId = ReadString(reader);
                if (!timerIds.Add(timerId))
                    throw new InvalidDataException(
                        $"Duplicate logical timer '{timerId}'.");
                var due = reader.ReadInt64();
                var period = reader.ReadInt64();
                if (period < 0)
                    throw new InvalidDataException(
                        "A logical timer period cannot be negative.");
                timers[timerIndex] = new ZLinkRelocationLogicalTimer(
                    timerId,
                    due,
                    period,
                    ReadBytes(reader, MaxFieldBytes));
            }
            participants[participantIndex] = new ZLinkRelocationParticipantEnvelope(
                key,
                objectKind,
                objectGeneration,
                ownerGeneration,
                state,
                jobs,
                timers,
                ReadBytes(reader, MaxFieldBytes),
                ReadBytes(reader, MaxFieldBytes));
        }
        if (input.Position != input.Length)
            throw new InvalidDataException(
                "The relocation root contains trailing bytes.");

        var envelope = HydrateCanonicalParticipantIdentity(
            new ZLinkRelocationEnvelope(
            aggregateId,
            aggregateGeneration,
            decodedInventoryDigest,
            participants));
        ValidateEnvelope(envelope);
        return envelope;
    }

    private static ZLinkRelocationEnvelope HydrateCanonicalParticipantIdentity(
        ZLinkRelocationEnvelope envelope)
    {
        var participants =
            new ZLinkRelocationParticipantEnvelope[envelope.Participants.Count];
        for (var index = 0; index < participants.Length; index++)
        {
            var participant = envelope.Participants[index];
            try
            {
                _ = ZLinkCanonicalParticipantRecoveryCodec.Decode(
                    participant.RecoveryPayload.Span);
            }
            catch (Exception error) when (error is InvalidDataException
                                          or EndOfStreamException)
            {
                return envelope;
            }
            participants[index] = participant with
            {
                CanonicalParticipantId = checked((ulong)index + 1)
            };
        }
        Span<byte> id = stackalloc byte[16];
        envelope.AggregateId.TryWriteBytes(
            id,
            bigEndian: true,
            out _);
        return envelope with
        {
            Participants = participants,
            CanonicalRelocationHigh =
                BinaryPrimitives.ReadUInt64BigEndian(id[..8]),
            CanonicalRelocationLow =
                BinaryPrimitives.ReadUInt64BigEndian(id[8..])
        };
    }

    private static ZLinkRelocationEnvelope DecodeCanonical(
        ReadOnlyMemory<byte> encoded,
        ReadOnlyMemory<byte> inventoryDigest)
    {
        ServiceWirePilotCodec.RelocationEnvelopeV1 generated;
        try
        {
            generated = ServiceWirePilotCodec.DecodeRelocationEnvelopeV1(
                encoded.ToArray());
        }
        catch (DecoderFallbackException error)
        {
            throw new InvalidDataException(
                "The relocation envelope contains invalid UTF-8.",
                error);
        }
        var canonical = ServiceWirePilotCodec.EncodeRelocationEnvelopeV1(
            generated);
        var (objectKind, objectId, objectGeneration, ownerGeneration) =
            ProjectObject(generated.Object);
        var authorityKey = objectKind switch
        {
            ZLinkPlacementObjectKind.Actor =>
                ZLinkAuthorityKeyCodec.EncodeActor(objectId),
            ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot =>
                ZLinkAuthorityKeyCodec.EncodeSpot(objectId),
            _ => throw new InvalidDataException("The relocation object kind is invalid.")
        };
        var participantIds = generated.ApplicationStates
            .Select(static state => state.ParticipantId)
            .ToArray();
        var jobs = participantIds.ToDictionary(
            static id => id,
            static _ => new List<ZLinkRelocationQueuedJob>());
        var acceptedOrders = new HashSet<(ulong ParticipantId, ulong Order)>();
        var operations = new HashSet<CanonicalSavedWorkOperation>();
        foreach (var work in generated.SavedWork)
        {
            if (!acceptedOrders.Add((work.ParticipantId, work.Order)))
                throw new InvalidDataException(
                    "The relocation saved-work order is invalid.");
            var reader = new CanonicalReader(work.FrozenRecord);
            var projection = ReadCanonicalFrozenRecordProjection(
                ref reader,
                out var operation);
            reader.RequireEnd("relocation frozen record");
            if (operation is { } exactOperation
                && !operations.Add(exactOperation))
                throw new InvalidDataException(
                    "The relocation saved work contains a duplicate operation identity.");
            jobs[work.ParticipantId].Add(new ZLinkRelocationQueuedJob(
                work.Order,
                work.FrozenRecord)
            {
                RequestSource = projection?.Source,
                CanonicalRequest = projection
            });
        }

        var timers = participantIds.ToDictionary(
            static id => id,
            static _ => new List<CanonicalTimerProjection>());
        foreach (var timer in generated.TimerRegistrations)
        {
            if (timer.PeriodMilliseconds > long.MaxValue
                || timer.NextScheduledAtUnixMilliseconds > long.MaxValue)
                throw new InvalidDataException(
                    "The relocation timer registration is invalid.");
            timers[timer.ParticipantId].Add(new CanonicalTimerProjection(
                timer.Name,
                checked((long)timer.NextScheduledAtUnixMilliseconds),
                checked((long)timer.PeriodMilliseconds),
                timer.HandlerType,
                timer.OverrunPolicy,
                timer.MaxCatchUpTicks,
                timer.StopOnUnhandledException,
                timer.LastCompletedDeliveryIndex,
                timer.LastCompletedScheduledIndex,
                checked((long)timer.NextScheduledAtUnixMilliseconds)));
        }
        foreach (var pending in generated.PendingTimerTicks)
        {
            if (!acceptedOrders.Add((pending.ParticipantId, pending.Order))
                || pending.ScheduledAtUnixMilliseconds > long.MaxValue)
                throw new InvalidDataException(
                    "The pending relocation timer tick is invalid.");
            var timer = timers[pending.ParticipantId].Single(candidate =>
                string.Equals(
                    candidate.TimerId,
                    pending.TimerName,
                    StringComparison.Ordinal));
            timer.AppendPendingTick(new ZLinkCanonicalPendingTimerTick(
                pending.Order,
                pending.DeliveryIndex,
                pending.ScheduledIndex,
                checked((long)pending.ScheduledAtUnixMilliseconds),
                pending.SkippedTicks));
        }

        var participants = generated.ApplicationStates.Select((state, index) =>
            new ZLinkRelocationParticipantEnvelope(
                index == 0
                    ? authorityKey
                    : new ZLinkAuthorityKey(
                        $"{authorityKey.Value}#participant:{state.ParticipantId}"),
                objectKind,
                objectGeneration,
                ownerGeneration,
                state.Payload,
                jobs[state.ParticipantId],
                timers[state.ParticipantId]
                    .Select(static timer => timer.ToRelocationTimer())
                    .ToArray(),
                RecoveryPayload: ReadOnlyMemory<byte>.Empty,
                CompletionPayload: ReadOnlyMemory<byte>.Empty)
            {
                CanonicalParticipantId = state.ParticipantId
            })
            .ToArray();
        var digest = inventoryDigest.IsEmpty
            ? new byte[32]
            : inventoryDigest.ToArray();
        if (digest.Length != 32)
            throw new InvalidDataException(
                "The relocation inventory digest must contain 32 bytes.");
        var result = new ZLinkRelocationEnvelope(
            RelocationGuid(generated.RelocationHigh, generated.RelocationLow),
            generated.ApplicationVersion == 0
                ? 1UL
                : checked((ulong)generated.ApplicationVersion),
            digest,
            participants)
        {
            CanonicalLogicalStream = canonical,
            CanonicalRelocationHigh = generated.RelocationHigh,
            CanonicalRelocationLow = generated.RelocationLow,
            CanonicalApplicationVersion = generated.ApplicationVersion
        };
        ValidateEnvelope(result);
        return result;
    }

    private static byte[] EncodeCanonical(ReadOnlyMemory<byte> encoded)
    {
        var generated = ServiceWirePilotCodec.DecodeRelocationEnvelopeV1(
            encoded.ToArray());
        return ServiceWirePilotCodec.EncodeRelocationEnvelopeV1(generated);
    }

    private static (
        ZLinkPlacementObjectKind Kind,
        string Id,
        ulong Generation,
        ulong OwnerGeneration) ProjectObject(
            ServiceWirePilotCodec.RelocationObjectIdentity value) => value switch
    {
        ServiceWirePilotCodec.RelocationActorIdentity actor =>
            (ZLinkPlacementObjectKind.Actor,
                actor.ActorId,
                actor.ObjectGeneration,
                actor.ExpectedAuthorityOwnerGeneration),
        ServiceWirePilotCodec.RelocationUserSpotIdentity spot =>
            (ZLinkPlacementObjectKind.UserSpot,
                spot.SpotId,
                spot.ObjectGeneration,
                spot.ExpectedAuthorityOwnerGeneration),
        ServiceWirePilotCodec.RelocationInstanceSpotIdentity spot =>
            (ZLinkPlacementObjectKind.InstanceSpot,
                spot.SpotId,
                spot.ObjectGeneration,
                1),
        _ => throw new InvalidDataException(
            "The relocation object kind is invalid.")
    };

    private static Guid RelocationGuid(ulong high, ulong low)
    {
        Span<byte> bytes = stackalloc byte[16];
        BinaryPrimitives.WriteUInt64BigEndian(bytes, high);
        BinaryPrimitives.WriteUInt64BigEndian(bytes[8..], low);
        return new Guid(bytes, bigEndian: true);
    }


    private static void ReadCanonicalFrozenRecord(ref CanonicalReader reader)
    {
        var recordKind = reader.ReadByte();
        if (recordKind is < 1 or > 14)
            throw new InvalidDataException("The relocation saved work record kind is invalid.");
        var sourceKind = reader.ReadByte();
        if (sourceKind is < 1 or > 4)
            throw new InvalidDataException("The relocation saved work source kind is invalid.");
        if (recordKind is 8 or 11 or 12 or 13 && sourceKind != 1)
            throw new InvalidDataException(
                "Infrastructure relocation records require a node source.");
        var source = new CanonicalReader(reader.ReadBytes(reader.ReadUInt16()));
        _ = source.ReadText8();
        if (source.ReadUInt64() == 0)
            throw new InvalidDataException("The relocation saved work source generation is invalid.");
        _ = source.ReadText8();
        if (source.ReadUInt64() == 0)
            throw new InvalidDataException("The relocation saved work source lease is invalid.");
        if (sourceKind == 2)
            _ = source.ReadText8();
        else if (sourceKind is 3 or 4)
        {
            _ = source.ReadText8();
            if (source.ReadUInt64() == 0)
                throw new InvalidDataException("The relocation saved work Actor generation is invalid.");
            if (sourceKind == 4)
            {
                _ = source.ReadText8();
                if (source.ReadUInt64() == 0 || source.ReadUInt64() == 0)
                    throw new InvalidDataException("The relocation saved work binding is invalid.");
            }
        }
        source.RequireEnd("relocation saved work source");
        var hasMetadata = reader.ReadByte();
        if (hasMetadata > 1)
            throw new InvalidDataException("The relocation saved work metadata flag is invalid.");
        if (hasMetadata == 1 && recordKind is not (1 or 2 or 3 or 4 or 5 or 6 or 7 or 9 or 10 or 14))
            throw new InvalidDataException("The relocation saved work metadata is forbidden for this record kind.");
        if (hasMetadata == 1)
            ReadCanonicalMetadata(ref reader);
        var operationHigh = reader.ReadUInt64();
        var operationLow = reader.ReadUInt64();
        var operationKind = reader.ReadUInt32();
        if (operationKind > 15)
            throw new InvalidDataException("The relocation saved work operation kind is invalid.");
        var operationNonzero = operationHigh != 0 || operationLow != 0;
        if (!IsCanonicalFrozenOperation(recordKind, operationKind, operationNonzero))
            throw new InvalidDataException("The relocation saved work operation identity is invalid.");
        var replyRoute = new CanonicalReader(reader.ReadBytes(reader.ReadUInt16()));
        if (operationKind is 1 or 2 or 3 or 4 or 12)
        {
            if (replyRoute.ReadUInt64() == 0)
                throw new InvalidDataException("The relocation reply route is invalid.");
        }
        replyRoute.RequireEnd("relocation reply route");
        ReadCanonicalFrozenRecordBody(ref reader, recordKind, operationKind,
            operationNonzero);
    }

    private static ZLinkCanonicalAcceptedRequest?
        ReadCanonicalFrozenRecordProjection(
            ref CanonicalReader reader,
            out CanonicalSavedWorkOperation? operation)
    {
        var recordKind = reader.ReadByte();
        if (recordKind is < 1 or > 14)
            throw new InvalidDataException("The relocation saved work record kind is invalid.");
        var sourceKind = reader.ReadByte();
        if (sourceKind is < 1 or > 4)
            throw new InvalidDataException("The relocation saved work source kind is invalid.");
        var source = new CanonicalReader(reader.ReadBytes(reader.ReadUInt16()));
        var sourceNodeRid = source.ReadText8();
        var sourceNodeGeneration = source.ReadUInt64();
        var sourceOwnerId = source.ReadText8();
        var sourceOwnerLease = source.ReadUInt64();
        string? sourceSpotId = null;
        if (sourceKind == 2)
            sourceSpotId = source.ReadText8();
        else if (sourceKind is 3 or 4)
        {
            _ = source.ReadText8();
            if (source.ReadUInt64() == 0)
                throw new InvalidDataException("The relocation saved work Actor generation is invalid.");
            if (sourceKind == 4)
            {
                _ = source.ReadText8();
                if (source.ReadUInt64() == 0 || source.ReadUInt64() == 0)
                    throw new InvalidDataException("The relocation saved work binding is invalid.");
            }
        }
        source.RequireEnd("relocation saved work source");
        if (sourceNodeGeneration == 0 || sourceOwnerLease == 0)
            throw new InvalidDataException("The relocation saved work source fence is invalid.");
        var hasMetadata = reader.ReadByte();
        if (hasMetadata > 1)
            throw new InvalidDataException("The relocation saved work metadata flag is invalid.");
        var metadata = hasMetadata == 1
            ? ReadCanonicalMetadataProjection(ref reader)
            : ZLinkMessageMetadata.Empty;
        var operationHigh = reader.ReadUInt64();
        var operationLow = reader.ReadUInt64();
        operation = operationHigh == 0 && operationLow == 0
            ? null
            : new CanonicalSavedWorkOperation(
                sourceOwnerId,
                sourceOwnerLease,
                sourceNodeRid,
                sourceNodeGeneration,
                operationHigh,
                operationLow);
        var operationKind = reader.ReadUInt32();
        if (operationKind > 15)
            throw new InvalidDataException("The relocation saved work operation kind is invalid.");
        var replyRoute = new CanonicalReader(reader.ReadBytes(reader.ReadUInt16()));
        var replyRouteId = operationKind is 1 or 2 or 3 or 4 or 12
            ? replyRoute.ReadUInt64()
            : 0;
        replyRoute.RequireEnd("relocation reply route");
        if (recordKind is not (5 or 6 or 9 or 10))
        {
            ReadCanonicalFrozenRecordBody(ref reader, recordKind, operationKind,
                operationHigh != 0 || operationLow != 0);
            return null;
        }
        var targetSpotId = reader.ReadText8();
        var targetSpotGeneration = reader.ReadUInt64();
        var targetNodeRid = reader.ReadText8();
        var targetNodeGeneration = reader.ReadUInt64();
        var targetAuthorityOwnerGeneration = reader.ReadUInt64();
        var targetOwnerLeaseGeneration = reader.ReadUInt64();
        var payload = ReadCanonicalApplicationPayloadProjection(ref reader);
        return new ZLinkCanonicalAcceptedRequest(
            new ZLinkCanonicalRequestSourceFence(
                sourceOwnerId,
                sourceOwnerLease,
                sourceNodeRid,
                sourceNodeGeneration),
            sourceSpotId,
            operationHigh,
            operationLow,
            replyRouteId,
            targetSpotId,
            targetSpotGeneration,
            targetNodeRid,
            targetNodeGeneration,
            targetAuthorityOwnerGeneration,
            targetOwnerLeaseGeneration,
            metadata,
            payload);
    }

    private readonly record struct CanonicalSavedWorkOperation(
        string SourceOwnerId,
        ulong SourceOwnerLeaseGeneration,
        string SourceNodeRid,
        ulong SourceNodeGeneration,
        ulong OperationHigh,
        ulong OperationLow);

    private static void ReadCanonicalFrozenRecordBody(
        ref CanonicalReader reader,
        byte recordKind,
        uint operationKind,
        bool operationNonzero)
    {
        switch (recordKind)
        {
            case 1 or 2:
                ReadCanonicalApplicationPayload(ref reader);
                return;
            case 3 or 4:
                _ = reader.ReadText8();
                ReadCanonicalApplicationPayload(ref reader);
                return;
            case 5 or 6:
                ReadCanonicalSpotFence(ref reader);
                ReadCanonicalApplicationPayload(ref reader);
                return;
            case 7:
                _ = reader.ReadText8();
                _ = reader.ReadText8();
                ReadCanonicalApplicationPayload(ref reader);
                return;
            case 8:
                ReadCanonicalActorControl(ref reader);
                return;
            case 9 or 10:
                ReadCanonicalActorFence(ref reader);
                ReadCanonicalApplicationPayload(ref reader);
                return;
            case 11:
                //  Schema terminal-failure-integrity: the frozen completion's
                //  terminal/failure pair must match the schema table, and a
                //  failure terminal carries no application payload.
                var completionTerminal = reader.ReadUInt32();
                var completionFailure = reader.ReadUInt32();
                if (!IsCanonicalTerminalResult(completionTerminal)
                    || !ServiceWireConstants.ValidTerminalFailure(
                        completionTerminal, completionFailure))
                    throw new InvalidDataException("The saved-work terminal result is invalid.");
                var hasPayload = reader.ReadByte();
                if (hasPayload > 1)
                    throw new InvalidDataException("The saved-work payload flag is invalid.");
                if (hasPayload == 1)
                {
                    if (completionTerminal != 0)
                        throw new InvalidDataException(
                            "A failed saved-work completion cannot carry a payload.");
                    ReadCanonicalApplicationPayload(ref reader);
                }
                return;
            case 12:
                ReadCanonicalSendReadyDestination(ref reader);
                return;
            case 13:
                ReadCanonicalRelocationControl(ref reader);
                return;
            case 14:
                ReadCanonicalInstanceRoute(ref reader);
                var sourceGeneration = reader.ReadUInt64();
                var instanceOperation = reader.ReadByte();
                if (sourceGeneration == 0 || instanceOperation is < 1 or > 2
                    || instanceOperation == 1 && (operationKind != 0 || operationNonzero)
                    || instanceOperation == 2 && (operationKind != 12 || !operationNonzero))
                    throw new InvalidDataException("The Instance activation record is invalid.");
                ReadCanonicalApplicationPayload(ref reader);
                return;
            default:
                throw new InvalidDataException("The relocation saved work record kind is invalid.");
        }
    }

    private static bool IsCanonicalFrozenOperation(
        byte recordKind, uint operationKind, bool operationNonzero) => recordKind switch
    {
        1 or 3 or 7 or 12 or 13 => operationKind == 0 && !operationNonzero,
        2 => operationKind == 1 && operationNonzero,
        4 => operationKind == 2 && operationNonzero,
        5 or 9 => operationKind == 0 && operationNonzero,
        6 => operationKind == 3 && operationNonzero,
        8 => operationKind == 0
            ? !operationNonzero
            : operationKind is 6 or 7 or 8 && operationNonzero,
        10 => operationKind == 4 && operationNonzero,
        11 => operationKind is >= 1 and <= 15 && operationNonzero,
        14 => operationKind is 0 or 12
              && operationNonzero == (operationKind == 12),
        _ => false
    };

    private static void ReadCanonicalActorControl(ref CanonicalReader reader)
    {
        var lifecycle = reader.ReadByte();
        if (lifecycle is < 1 or > 5)
            throw new InvalidDataException("The Actor lifecycle kind is invalid.");
        var body = new CanonicalReader(reader.ReadBytes(reader.ReadUInt16()));
        if (lifecycle == 2)
        {
            ReadCanonicalOptionalActorMembership(ref body);
            ReadCanonicalActorMembership(ref body);
        }
        else if (lifecycle == 3)
        {
            ReadCanonicalActorMembership(ref body);
            ReadCanonicalActorMembership(ref body);
        }
        else
        {
            ReadCanonicalActorMembership(ref body);
        }
        body.RequireEnd("Actor lifecycle control");
    }

    private static void ReadCanonicalOptionalActorMembership(ref CanonicalReader reader)
    {
        var present = reader.ReadByte();
        if (present > 1)
            throw new InvalidDataException("The Actor membership presence flag is invalid.");
        var body = new CanonicalReader(reader.ReadBytes(reader.ReadUInt16()));
        if (present == 1)
            ReadCanonicalActorMembership(ref body);
        body.RequireEnd("optional Actor membership");
    }

    private static void ReadCanonicalActorMembership(ref CanonicalReader reader)
    {
        ReadCanonicalActorRef(ref reader);
        ReadCanonicalSpotRef(ref reader);
    }

    private static void ReadCanonicalActorRef(ref CanonicalReader reader)
    {
        _ = reader.ReadText8();
        if (reader.ReadUInt64() == 0)
            throw new InvalidDataException("The Actor generation is invalid.");
    }

    private static void ReadCanonicalSpotRef(ref CanonicalReader reader)
    {
        _ = reader.ReadText8();
        if (reader.ReadUInt64() == 0)
            throw new InvalidDataException("The Spot generation is invalid.");
    }

    private static void ReadCanonicalActorFence(ref CanonicalReader reader)
    {
        ReadCanonicalActorRef(ref reader);
        _ = reader.ReadText8();
        if (reader.ReadUInt64() == 0
            || reader.ReadUInt64() == 0
            || reader.ReadUInt64() == 0)
            throw new InvalidDataException("The Actor authority fence is invalid.");
    }

    private static void ReadCanonicalSendReadyDestination(ref CanonicalReader reader)
    {
        var kind = reader.ReadByte();
        if (kind is < 1 or > 5)
            throw new InvalidDataException("The send-ready destination kind is invalid.");
        var body = new CanonicalReader(reader.ReadBytes(reader.ReadUInt16()));
        switch (kind)
        {
            case 1 or 2:
                _ = body.ReadText8();
                break;
            case 3:
                ReadCanonicalSpotFence(ref body);
                break;
            case 4:
                ReadCanonicalActorFence(ref body);
                break;
            case 5:
                ReadCanonicalActorFence(ref body);
                if (body.ReadUInt64() == 0)
                    throw new InvalidDataException("The session binding generation is invalid.");
                break;
        }
        body.RequireEnd("send-ready destination");
    }

    private static void ReadCanonicalRelocationControl(ref CanonicalReader reader)
    {
        if (reader.ReadByte() > 9 || reader.ReadByte() is < 1 or > 3)
            throw new InvalidDataException("The relocation control discriminator is invalid.");
        var relocationId = reader.ReadBytes(16);
        if (relocationId.Span.IndexOfAnyExcept((byte)0) < 0)
            throw new InvalidDataException("The relocation control id is invalid.");
        ReadCanonicalRelocationObject(ref reader);
        //  Schema terminal-failure-integrity: the relocation-control-data
        //  terminal/failure pair must match the schema table.
        var controlTerminal = reader.ReadUInt32();
        var controlFailure = reader.ReadUInt32();
        if (!IsCanonicalTerminalResult(controlTerminal)
            || !ServiceWireConstants.ValidTerminalFailure(
                controlTerminal, controlFailure))
            throw new InvalidDataException("The relocation control result is invalid.");
    }

    private static void ReadCanonicalRelocationObject(ref CanonicalReader reader)
    {
        var kind = reader.ReadByte();
        if (kind is < 1 or > 3)
            throw new InvalidDataException("The relocation object kind is invalid.");
        var body = new CanonicalReader(reader.ReadBytes(reader.ReadUInt16()));
        if (kind == 1)
        {
            ReadCanonicalActorRef(ref body);
            if (body.ReadUInt64() == 0)
                throw new InvalidDataException("The Actor owner generation is invalid.");
        }
        else if (kind == 2)
        {
            ReadCanonicalSpotRef(ref body);
            if (body.ReadUInt64() == 0)
                throw new InvalidDataException("The Spot owner generation is invalid.");
        }
        else
        {
            _ = body.ReadText8();
            _ = body.ReadText8();
            if (body.ReadUInt64() == 0)
                throw new InvalidDataException("The Instance Spot generation is invalid.");
        }
        body.RequireEnd("relocation object identity");
    }

    private static void ReadCanonicalSpotFence(ref CanonicalReader reader)
    {
        _ = reader.ReadText8();
        if (reader.ReadUInt64() == 0)
            throw new InvalidDataException("The relocation Spot generation is invalid.");
        _ = reader.ReadText8();
        if (reader.ReadUInt64() == 0
            || reader.ReadUInt64() == 0
            || reader.ReadUInt64() == 0)
            throw new InvalidDataException("The relocation Spot authority fence is invalid.");
    }

    private static void ReadCanonicalInstanceRoute(ref CanonicalReader reader)
    {
        var kind = reader.ReadByte();
        if (kind is < 1 or > 2)
            throw new InvalidDataException("The Instance relocation route version is invalid.");
        var route = new CanonicalReader(reader.ReadBytes(reader.ReadUInt16()));
        _ = route.ReadText8();
        if (route.ReadUInt64() == 0)
            throw new InvalidDataException("The Instance relocation node generation is invalid.");
        _ = route.ReadText8();
        if (kind == 1)
        {
            if (route.ReadUInt64() == 0)
                throw new InvalidDataException("The Instance relocation object generation is invalid.");
            _ = route.ReadText8();
            if (route.ReadUInt64() == 0 || route.ReadUInt64() == 0)
                throw new InvalidDataException("The Instance relocation authority fence is invalid.");
            _ = route.ReadText16();
        }
        else
        {
            _ = route.ReadText8();
            _ = route.ReadText8();
            _ = route.ReadText8();
            if (route.ReadUInt64() == 0)
                throw new InvalidDataException(
                    "The Instance relocation deadline is invalid.");
        }
        route.RequireEnd("Instance relocation route");
    }

    private static void ReadCanonicalApplicationPayload(ref CanonicalReader reader)
    {
        if (reader.ReadByte() != 1)
            throw new InvalidDataException("The relocation application payload version is invalid.");
        var body = new CanonicalReader(reader.ReadBytes(checked((int)reader.ReadUInt32())));
        _ = body.ReadText8();
        _ = body.ReadText8();
        _ = body.ReadBytes(checked((int)body.ReadUInt32()));
        body.RequireEnd("relocation application payload");
    }

    private static ZLinkCanonicalApplicationPayload
        ReadCanonicalApplicationPayloadProjection(ref CanonicalReader reader)
    {
        if (reader.ReadByte() != 1)
            throw new InvalidDataException("The relocation application payload version is invalid.");
        var body = new CanonicalReader(reader.ReadBytes(checked((int)reader.ReadUInt32())));
        var packetName = body.ReadText8();
        var contentType = body.ReadText8();
        var payload = body.ReadBytes(checked((int)body.ReadUInt32()));
        body.RequireEnd("relocation application payload");
        return new ZLinkCanonicalApplicationPayload(packetName, contentType, payload);
    }

    private static void ReadCanonicalMetadata(ref CanonicalReader reader)
    {
        if (reader.ReadByte() != 1)
            throw new InvalidDataException("The relocation metadata version is invalid.");
        var count = reader.ReadByte();
        var keys = new HashSet<string>(StringComparer.Ordinal);
        for (var index = 0; index < count; index++)
        {
            if (!keys.Add(reader.ReadText8()))
                throw new InvalidDataException("The relocation metadata contains a duplicate key.");
            _ = reader.ReadText16();
        }
    }

    private static ZLinkMessageMetadata ReadCanonicalMetadataProjection(
        ref CanonicalReader reader)
    {
        if (reader.ReadByte() != 1)
            throw new InvalidDataException("The relocation metadata version is invalid.");
        var count = reader.ReadByte();
        var values = new Dictionary<string, string>(count, StringComparer.Ordinal);
        for (var index = 0; index < count; index++)
            if (!values.TryAdd(reader.ReadText8(), reader.ReadText16()))
                throw new InvalidDataException(
                    "The relocation metadata contains a duplicate key.");
        return new ZLinkMessageMetadata(values);
    }

    private static bool IsCanonicalTerminalResult(uint value) =>
        value == 0 || value is >= 101 and <= 113;

    private sealed class CanonicalTimerProjection(
        string timerId,
        long dueUnixTimeMilliseconds,
        long periodMilliseconds,
        string handlerType,
        byte overrunPolicy,
        ulong maxCatchUpTicks,
        bool stopOnUnhandledException,
        ulong lastCompletedDeliveryIndex,
        ulong lastCompletedScheduledIndex,
        long nextScheduledAtUnixMilliseconds)
    {
        private ZLinkCanonicalPendingTimerTick? _pendingTick;

        internal string TimerId { get; } = timerId;

        internal void AppendPendingTick(
            ZLinkCanonicalPendingTimerTick pendingTick)
        {
            _pendingTick = pendingTick;
        }

        internal ZLinkRelocationLogicalTimer ToRelocationTimer() =>
            new(
                TimerId,
                dueUnixTimeMilliseconds,
                periodMilliseconds,
                ReadOnlyMemory<byte>.Empty)
            {
                PendingAcceptedSequence = _pendingTick?.AcceptedSequence ?? 0,
                CanonicalTimer = new ZLinkCanonicalLogicalTimer(
                    handlerType,
                    overrunPolicy,
                    maxCatchUpTicks,
                    stopOnUnhandledException,
                    lastCompletedDeliveryIndex,
                    lastCompletedScheduledIndex,
                    nextScheduledAtUnixMilliseconds,
                    _pendingTick)
            };
    }

    private ref struct VersionedMemoryReader(
        ReadOnlyMemory<byte> source)
    {
        private readonly ReadOnlyMemory<byte> _source = source;
        private int _offset;

        internal int Remaining => _source.Length - _offset;

        internal byte ReadByte() => ReadBytes(1).Span[0];

        internal ushort ReadUInt16() =>
            BinaryPrimitives.ReadUInt16LittleEndian(
                ReadBytes(sizeof(ushort)).Span);

        internal uint ReadUInt32() =>
            BinaryPrimitives.ReadUInt32LittleEndian(
                ReadBytes(sizeof(uint)).Span);

        internal ulong ReadUInt64() =>
            BinaryPrimitives.ReadUInt64LittleEndian(
                ReadBytes(sizeof(ulong)).Span);

        internal long ReadInt64() =>
            BinaryPrimitives.ReadInt64LittleEndian(
                ReadBytes(sizeof(long)).Span);

        internal int ReadInt32() =>
            BinaryPrimitives.ReadInt32LittleEndian(
                ReadBytes(sizeof(int)).Span);

        internal string ReadString()
        {
            var size = ReadUInt16();
            if (size == 0)
                throw new InvalidDataException(
                    "A relocation string is empty.");
            try
            {
                return StrictUtf8.GetString(ReadBytes(size).Span);
            }
            catch (DecoderFallbackException error)
            {
                throw new InvalidDataException(
                    "A relocation string contains invalid UTF-8.",
                    error);
            }
        }

        internal ReadOnlyMemory<byte> ReadByteField(int maximum)
        {
            var size = ReadInt32();
            if (size < 0 || size > maximum)
                throw new InvalidDataException(
                    "A relocation byte field exceeds its bound.");
            return ReadBytes(size);
        }

        internal int ReadCount(int maximum, string name)
        {
            var count = ReadInt32();
            if (count < 0 || count > maximum)
                throw new InvalidDataException(
                    $"The relocation {name} count exceeds its bound.");
            return count;
        }

        internal int ReadCountWithinRemaining(
            int minimumItemBytes,
            string name)
        {
            var count = ReadCount(int.MaxValue, name);
            if (count > Remaining / minimumItemBytes)
                throw new InvalidDataException(
                    $"The relocation {name} count exceeds the remaining payload.");
            return count;
        }

        internal ReadOnlyMemory<byte> ReadBytes(int length)
        {
            if (length < 0 || length > _source.Length - _offset)
                throw new EndOfStreamException();
            var value = _source.Slice(_offset, length);
            _offset += length;
            return value;
        }

        internal void RequireEnd(string field)
        {
            if (_offset != _source.Length)
                throw new InvalidDataException(
                    $"The {field} contains trailing bytes.");
        }
    }

    private ref struct CanonicalReader(ReadOnlyMemory<byte> source)
    {
        private readonly ReadOnlyMemory<byte> _source = source;
        private int _offset;

        internal byte ReadByte() => ReadBytes(1).Span[0];

        internal ushort ReadUInt16() =>
            BinaryPrimitives.ReadUInt16BigEndian(
                ReadBytes(sizeof(ushort)).Span);

        internal uint ReadUInt32() =>
            BinaryPrimitives.ReadUInt32BigEndian(
                ReadBytes(sizeof(uint)).Span);

        internal ulong ReadUInt64() =>
            BinaryPrimitives.ReadUInt64BigEndian(
                ReadBytes(sizeof(ulong)).Span);

        internal string ReadText8()
        {
            var length = ReadByte();
            if (length == 0)
                throw new InvalidDataException("A relocation string is empty.");
            return DecodeText(ReadBytes(length).Span);
        }

        internal string ReadText16()
        {
            var length = ReadUInt16();
            if (length == 0)
                throw new InvalidDataException("A relocation string is empty.");
            return DecodeText(ReadBytes(length).Span);
        }

        private static string DecodeText(ReadOnlySpan<byte> encoded)
        {
            try
            {
                return StrictUtf8.GetString(encoded);
            }
            catch (DecoderFallbackException error)
            {
                throw new InvalidDataException(
                    "A relocation string contains invalid UTF-8.",
                    error);
            }
        }

        internal ReadOnlyMemory<byte> ReadBytes(int length)
        {
            if (length < 0 || length > _source.Length - _offset)
                throw new EndOfStreamException();
            var value = _source.Slice(_offset, length);
            _offset += length;
            return value;
        }

        internal void RequireEnd(string field)
        {
            if (_offset != _source.Length)
                throw new InvalidDataException($"The {field} contains trailing bytes.");
        }
    }

    private static void ValidateEnvelope(ZLinkRelocationEnvelope envelope)
    {
        if (envelope.AggregateId == Guid.Empty)
            throw new ArgumentException(
                "A relocation aggregate id must not be empty.",
                nameof(envelope));
        if (envelope.AggregateGeneration is 0 or > long.MaxValue)
            throw new ArgumentOutOfRangeException(
                nameof(envelope),
                "A relocation aggregate generation must be a non-zero signed 63-bit value.");
        if (envelope.InventoryDigest.Length != 32)
            throw new ArgumentException(
                "A relocation inventory digest must contain 32 bytes.",
                nameof(envelope));
        if (envelope.Participants.Count < 1)
            throw new ArgumentOutOfRangeException(
                nameof(envelope),
                "A relocation aggregate must contain at least one participant.");

        var keys = new HashSet<string>(StringComparer.Ordinal);
        foreach (var participant in envelope.Participants)
        {
            ArgumentException.ThrowIfNullOrWhiteSpace(participant.AuthorityKey.Value);
            if (!keys.Add(participant.AuthorityKey.Value))
                throw new ArgumentException(
                    $"Duplicate relocation participant '{participant.AuthorityKey.Value}'.",
                    nameof(envelope));
            if (participant.ObjectGeneration is 0 or > long.MaxValue
                || participant.AuthorityOwnerGeneration is 0 or > long.MaxValue)
                throw new ArgumentOutOfRangeException(
                    nameof(envelope),
                    "Relocation participant generations must be non-zero signed 63-bit values.");
            if (participant.LogicalTimers.Count > MaxTimerItems)
                throw new ArgumentOutOfRangeException(
                    nameof(envelope),
                    "A relocation participant contains too many timers.");

            ulong previousSequence = 0;
            foreach (var job in participant.AcceptedJobs)
            {
                if (job.AcceptedSequence == 0
                    || job.AcceptedSequence <= previousSequence)
                    throw new ArgumentException(
                        "Accepted job sequences must be strictly increasing.",
                        nameof(envelope));
                previousSequence = job.AcceptedSequence;
            }
            var timerIds = new HashSet<string>(StringComparer.Ordinal);
            foreach (var timer in participant.LogicalTimers)
            {
                ArgumentException.ThrowIfNullOrWhiteSpace(timer.TimerId);
                if (!timerIds.Add(timer.TimerId))
                    throw new ArgumentException(
                        $"Duplicate logical timer '{timer.TimerId}'.",
                        nameof(envelope));
                if (timer.PeriodMilliseconds < 0)
                    throw new ArgumentOutOfRangeException(
                        nameof(envelope),
                        "A logical timer period cannot be negative.");
            }
        }
    }

    private static void WriteString(BinaryWriter writer, string value)
    {
        var encoded = Encoding.UTF8.GetBytes(value);
        if (encoded.Length is < 1 or > ushort.MaxValue)
            throw new ArgumentOutOfRangeException(
                nameof(value),
                "Relocation strings must be 1 to 65535 UTF-8 bytes.");
        writer.Write((ushort)encoded.Length);
        writer.Write(encoded);
    }

    private static string ReadString(BinaryReader reader)
    {
        var size = reader.ReadUInt16();
        if (size == 0)
            throw new InvalidDataException("A relocation string is empty.");
        return Encoding.UTF8.GetString(ReadExact(reader, size));
    }

    private static void WriteBytes(BinaryWriter writer, ReadOnlySpan<byte> value)
    {
        writer.Write(value.Length);
        writer.Write(value);
    }

    private static byte[] ReadBytes(BinaryReader reader, int maximum)
    {
        var size = reader.ReadInt32();
        if (size < 0 || size > maximum)
            throw new InvalidDataException(
                "A relocation byte field exceeds its bound.");
        return ReadExact(reader, size);
    }

    private static int ReadCount(BinaryReader reader, int maximum, string name)
    {
        var count = reader.ReadInt32();
        if (count < 0 || count > maximum)
            throw new InvalidDataException(
                $"The relocation {name} count exceeds its bound.");
        return count;
    }

    private static int ReadCountWithinRemaining(
        BinaryReader reader,
        Stream input,
        int minimumItemBytes,
        string name)
    {
        var count = ReadCount(reader, int.MaxValue, name);
        if (count > (input.Length - input.Position) / minimumItemBytes)
            throw new InvalidDataException(
                $"The relocation {name} count exceeds the remaining payload.");
        return count;
    }

    private static byte[] ReadExact(BinaryReader reader, int size)
    {
        var value = reader.ReadBytes(size);
        if (value.Length != size)
            throw new EndOfStreamException();
        return value;
    }

    private sealed class CountingStream : Stream
    {
        private long _length;

        public override bool CanRead => false;
        public override bool CanSeek => false;
        public override bool CanWrite => true;
        public override long Length => _length;
        public override long Position
        {
            get => Length;
            set => throw new NotSupportedException();
        }

        public override void Write(byte[] buffer, int offset, int count) =>
            _length = checked(_length + count);

        public override void Write(ReadOnlySpan<byte> buffer) =>
            _length = checked(_length + buffer.Length);

        public override void Flush() { }
        public override int Read(byte[] buffer, int offset, int count) =>
            throw new NotSupportedException();
        public override long Seek(long offset, SeekOrigin origin) =>
            throw new NotSupportedException();
        public override void SetLength(long value) =>
            throw new NotSupportedException();
    }

    private sealed class HashStream(IncrementalHash hash) : Stream
    {
        public override bool CanRead => false;
        public override bool CanSeek => false;
        public override bool CanWrite => true;
        public override long Length => throw new NotSupportedException();
        public override long Position
        {
            get => throw new NotSupportedException();
            set => throw new NotSupportedException();
        }

        public override void Write(byte[] buffer, int offset, int count) =>
            hash.AppendData(buffer, offset, count);

        public override void Write(ReadOnlySpan<byte> buffer) =>
            hash.AppendData(buffer);

        public override void Flush() { }
        public override int Read(byte[] buffer, int offset, int count) =>
            throw new NotSupportedException();
        public override long Seek(long offset, SeekOrigin origin) =>
            throw new NotSupportedException();
        public override void SetLength(long value) =>
            throw new NotSupportedException();
    }
}

internal static class ZLinkCrc32C
{
    internal static uint Compute(ReadOnlySpan<byte> data)
    {
        var crc = uint.MaxValue;
        Append(ref crc, data);
        return ~crc;
    }

    internal static void Append(
        ref uint crc,
        ReadOnlySpan<byte> data)
    {
        foreach (var value in data)
        {
            crc ^= value;
            for (var bit = 0; bit < 8; bit++)
                crc = (crc >> 1) ^ (0x82f63b78u & (uint)-(int)(crc & 1));
        }
    }
}
