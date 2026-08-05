using System.Buffers.Binary;
using System.Text;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkCanonicalSpotRelocationWriter
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    internal static ZLinkRelocationEnvelope CreateInitial(
        ZLinkRelocationEnvelope inventory,
        string spotId,
        string stableType,
        RoutingId targetNodeRid,
        long applicationVersion)
    {
        if (applicationVersion < 0)
            throw new ArgumentOutOfRangeException(nameof(applicationVersion));
        var ordered = inventory.Participants
            .OrderBy(static participant => participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot ? 0 : 1)
            .ThenBy(static participant => participant.AuthorityKey.Value,
                StringComparer.Ordinal)
            .ToArray();
        var spot = ordered[0];
        if (spot.ObjectKind is not (ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot))
            throw new ArgumentException("Canonical SPOT relocation has no root SPOT.", nameof(inventory));
        using var stream = new MemoryStream();
        var id = inventory.AggregateId.ToByteArray(bigEndian: true);
        stream.Write(id);
        stream.WriteByte(spot.ObjectKind == ZLinkPlacementObjectKind.InstanceSpot
            ? (byte)3 : (byte)2);
        using (var identity = new MemoryStream())
        {
            if (spot.ObjectKind == ZLinkPlacementObjectKind.InstanceSpot)
            {
                Text8(identity, stableType);
                Text8(identity, spotId);
                U64(identity, spot.ObjectGeneration);
            }
            else
            {
                Text8(identity, spotId);
                U64(identity, spot.ObjectGeneration);
                U64(identity, spot.AuthorityOwnerGeneration);
            }
            U16(stream, checked((ushort)identity.Length));
            identity.Position = 0;
            identity.CopyTo(stream);
        }
        I64(stream, applicationVersion);
        U32(stream, checked((uint)ordered.Length));
        for (var index = 0; index < ordered.Length; index++)
        {
            U64(stream, checked((ulong)index + 1));
            stream.WriteByte(ordered[index].RecoveryPayload.IsEmpty
                ? (byte)1 : (byte)2);
            using var state = new MemoryStream();
            U64(state, checked((ulong)ordered[index].ApplicationState.Length));
            state.Write(ordered[index].ApplicationState.Span);
            if (!ordered[index].RecoveryPayload.IsEmpty)
            {
                U64(state, checked((ulong)ordered[index].RecoveryPayload.Length));
                state.Write(ordered[index].RecoveryPayload.Span);
            }
            U64(stream, checked((ulong)state.Length));
            state.Position = 0;
            state.CopyTo(stream);
        }
        U32(stream, checked((uint)ordered.Length));
        for (var index = 0; index < ordered.Length; index++)
        {
            var participant = ordered[index];
            var acceptedBoundary = participant.AcceptedJobs
                .Select(static job => job.AcceptedSequence)
                .Concat(participant.LogicalTimers.Select(
                    static timer => timer.PendingAcceptedSequence))
                .DefaultIfEmpty(0UL)
                .Max();
            U64(stream, checked((ulong)index + 1));
            U64(stream, acceptedBoundary);
            U64(stream, 0);
        }
        U32(stream, checked((uint)ordered.Sum(
            static participant => participant.AcceptedJobs.Count)));
        for (var index = 0; index < ordered.Length; index++)
        {
            var participant = ordered[index];
            foreach (var job in participant.AcceptedJobs.OrderBy(
                         static job => job.AcceptedSequence))
            {
                U64(stream, checked((ulong)index + 1));
                U64(stream, job.AcceptedSequence);
                if (index == 0)
                {
                    WriteAcceptedRequest(
                        stream,
                        job,
                        spotId,
                        spot,
                        targetNodeRid);
                    continue;
                }
                if (!ZLinkRelocationEnvelopeCodec.TryValidateCanonicalFrozenRecord(
                        job.Payload.Span))
                    throw new ZLinkRelocationDataLostException(
                        $"Actor participant '{participant.AuthorityKey.Value}' accepted journal is malformed.");
                stream.Write(job.Payload.Span);
            }
        }
        var snapshots = spot.LogicalTimers
            .Select(timer => (Timer: timer,
                Snapshot: ZLinkSpotTimerRelocationCodec.Decode(timer)))
            .ToArray();
        U32(stream, checked((uint)snapshots.Length));
        foreach (var item in snapshots.OrderBy(static item => item.Timer.TimerId,
                     StringComparer.Ordinal))
        {
            var timer = item.Snapshot.Timer;
            U64(stream, 1);
            Text8(stream, timer.Name);
            Text8(stream, item.Snapshot.HandlerType.AssemblyQualifiedName
                          ?? item.Snapshot.HandlerType.FullName
                          ?? item.Snapshot.HandlerType.Name);
            U64(stream, checked((ulong)Math.Max(1,
                Math.Ceiling(timer.Period.TotalMilliseconds))));
            stream.WriteByte((byte)timer.Options.OverrunPolicy);
            U64(stream, checked((ulong)Math.Max(1, timer.Options.MaxCatchUpTicks)));
            stream.WriteByte(timer.Options.StopOnUnhandledException ? (byte)1 : (byte)0);
            U64(stream, timer.DeliveryIndex);
            U64(stream, timer.LastScheduledIndex);
            U64(stream, checked((ulong)(timer.NextScheduledAt
                ?? timer.StartedAt + timer.Period).ToUnixTimeMilliseconds()));
        }
        var pending = snapshots.Where(static item =>
                item.Snapshot.Timer.PendingTick.HasValue)
            .OrderBy(static item => item.Timer.PendingAcceptedSequence)
            .ToArray();
        U32(stream, checked((uint)pending.Length));
        foreach (var item in pending)
        {
            var tick = item.Snapshot.Timer.PendingTick!.Value;
            U64(stream, 1);
            U64(stream, item.Timer.PendingAcceptedSequence);
            Text8(stream, item.Timer.TimerId);
            U64(stream, tick.DeliveryIndex);
            U64(stream, tick.ScheduledIndex);
            U64(stream, checked((ulong)tick.ScheduledAt.ToUnixTimeMilliseconds()));
            U64(stream, tick.SkippedTicks);
        }
        U32(stream, 0);
        stream.Position = 0;
        var canonical = ZLinkRelocationEnvelopeCodec.Decode(
            stream, inventory.InventoryDigest);
        var projected = ordered.Select((participant, index) =>
        {
            var state = canonical.Participants[index];
            return participant with
            {
                AcceptedJobs = state.AcceptedJobs,
                LogicalTimers = state.LogicalTimers,
                CompletionPayload = participant.CompletionPayload,
                CanonicalParticipantId = state.CanonicalParticipantId,
                AcceptedBoundary = state.AcceptedBoundary,
                ReplayCursor = state.ReplayCursor,
                TerminalCompletions = state.TerminalCompletions
            };
        }).ToArray();
        return inventory with
        {
            Participants = projected,
            CanonicalLogicalStream = canonical.CanonicalLogicalStream,
            CanonicalLayout = canonical.CanonicalLayout,
            CanonicalRelocationHigh = canonical.CanonicalRelocationHigh,
            CanonicalRelocationLow = canonical.CanonicalRelocationLow,
            CanonicalApplicationVersion = canonical.CanonicalApplicationVersion
        };
    }

    private static void WriteAcceptedRequest(
        Stream stream,
        ZLinkRelocationQueuedJob job,
        string targetSpotId,
        ZLinkRelocationParticipantEnvelope spot,
        RoutingId targetNodeRid)
    {
        var journal = ZLinkSpotAcceptedJournal.Decode(job.Payload.Span);
        var source = job.RequestSource
                     ?? throw new ZLinkRelocationDataLostException(
                         "Accepted request source fence was not captured.");
        if (journal.OperationId == default || journal.SourceNodeGeneration == 0)
            throw new ZLinkRelocationDataLostException(
                "Accepted request operation fence was not captured.");
        var parts = journal.Parts.Select(static part => Message.From(part.Span)).ToArray();
        ZLinkEnvelopeHeader header;
        byte[] payload;
        try
        {
            header = ZLinkEnvelopeCodec.DecodeHeader(parts);
            if (parts.Length != 2)
                throw new InvalidDataException(
                    "Canonical accepted application payload must contain header and body parts.");
            payload = parts[1].ToArray();
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
        var request = journal.ReplyRouteId != 0;
        stream.WriteByte(request ? (byte)6 : (byte)5);
        stream.WriteByte(journal.SpotId is null ? (byte)1 : (byte)2);
        using (var sourceBody = new MemoryStream())
        {
            Text8(sourceBody, source.NodeRid);
            U64(sourceBody, source.NodeGeneration);
            Text8(sourceBody, source.OwnerId);
            U64(sourceBody, source.OwnerLeaseGeneration);
            if (journal.SpotId is not null)
                Text8(sourceBody, journal.SpotId);
            U16(stream, checked((ushort)sourceBody.Length));
            sourceBody.Position = 0;
            sourceBody.CopyTo(stream);
        }
        WriteMetadata(stream, journal.Metadata);
        U64(stream, journal.OperationId.High);
        U64(stream, journal.OperationId.Low);
        U32(stream, request ? 3U : 0U);
        U16(stream, request ? (ushort)sizeof(ulong) : (ushort)0);
        if (request)
            U64(stream, journal.ReplyRouteId);
        Text8(stream, targetSpotId);
        U64(stream, spot.ObjectGeneration);
        Text8(stream, targetNodeRid.ToHex());
        U64(stream, journal.TargetNodeGeneration);
        U64(stream, journal.AuthorityOwnerGeneration);
        U64(stream, journal.OwnerLeaseGeneration);
        WriteApplicationPayload(stream, header.MessageName, header.ContentType, payload);
    }

    private static void WriteMetadata(Stream stream, ZLinkMessageMetadata metadata)
    {
        if (metadata.Values.Count == 0)
        {
            stream.WriteByte(0);
            return;
        }
        if (metadata.Values.Count > byte.MaxValue)
            throw new InvalidDataException("Canonical metadata exceeds its entry bound.");
        stream.WriteByte(1);
        stream.WriteByte(1);
        stream.WriteByte(checked((byte)metadata.Values.Count));
        foreach (var pair in metadata.Values.OrderBy(static pair => pair.Key,
                     StringComparer.Ordinal))
        {
            Text8(stream, pair.Key);
            Text16(stream, pair.Value);
        }
    }

    private static void WriteApplicationPayload(
        Stream stream, string packetName, string contentType, ReadOnlySpan<byte> payload)
    {
        using var body = new MemoryStream();
        Text8(body, packetName);
        Text8(body, contentType);
        U32(body, checked((uint)payload.Length));
        body.Write(payload);
        stream.WriteByte(1);
        U32(stream, checked((uint)body.Length));
        body.Position = 0;
        body.CopyTo(stream);
    }

    private static void Text8(Stream stream, string value)
    {
        var bytes = StrictUtf8.GetBytes(value);
        if (bytes.Length is < 1 or > byte.MaxValue || bytes.AsSpan().Contains((byte)0))
            throw new ArgumentOutOfRangeException(nameof(value));
        stream.WriteByte(checked((byte)bytes.Length));
        stream.Write(bytes);
    }

    private static void Text16(Stream stream, string value)
    {
        var bytes = StrictUtf8.GetBytes(value);
        if (bytes.Length is < 1 or > ushort.MaxValue || bytes.AsSpan().Contains((byte)0))
            throw new ArgumentOutOfRangeException(nameof(value));
        U16(stream, checked((ushort)bytes.Length));
        stream.Write(bytes);
    }

    private static void U16(Stream stream, ushort value)
    {
        Span<byte> bytes = stackalloc byte[2];
        BinaryPrimitives.WriteUInt16BigEndian(bytes, value);
        stream.Write(bytes);
    }
    private static void U32(Stream stream, uint value)
    {
        Span<byte> bytes = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(bytes, value);
        stream.Write(bytes);
    }
    private static void U64(Stream stream, ulong value)
    {
        Span<byte> bytes = stackalloc byte[8];
        BinaryPrimitives.WriteUInt64BigEndian(bytes, value);
        stream.Write(bytes);
    }
    private static void I64(Stream stream, long value)
    {
        Span<byte> bytes = stackalloc byte[8];
        BinaryPrimitives.WriteInt64BigEndian(bytes, value);
        stream.Write(bytes);
    }
}
