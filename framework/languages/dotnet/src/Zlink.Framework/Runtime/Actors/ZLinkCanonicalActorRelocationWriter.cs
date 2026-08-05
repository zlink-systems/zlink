using System.Buffers.Binary;
using System.Text;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Actors;

/// <summary>
/// Projects the standalone Actor inventory into the canonical relocation root.
/// The accepted records remain byte-exact; only the root index and progress
/// sections are assembled here.
/// </summary>
internal static class ZLinkCanonicalActorRelocationWriter
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    internal static ZLinkRelocationEnvelope CreateInitial(
        ZLinkRelocationEnvelope inventory,
        long applicationVersion)
    {
        ArgumentNullException.ThrowIfNull(inventory);
        if (applicationVersion < 0)
            throw new ArgumentOutOfRangeException(nameof(applicationVersion));
        var participant = inventory.Participants.Single();
        if (participant.ObjectKind != ZLinkPlacementObjectKind.Actor)
            throw new ArgumentException(
                "Canonical standalone Actor relocation requires one Actor participant.",
                nameof(inventory));

        var accepted = participant.AcceptedJobs
            .OrderBy(static job => job.AcceptedSequence)
            .ToArray();
        var acceptedBoundary = accepted
            .Select(static job => job.AcceptedSequence)
            .DefaultIfEmpty(0UL)
            .Max();
        using var stream = new MemoryStream();
        stream.Write(inventory.AggregateId.ToByteArray(bigEndian: true));
        stream.WriteByte((byte)ZLinkPlacementObjectKind.Actor);
        using (var identity = new MemoryStream())
        {
            Text8(identity, participant.AuthorityKey.Value);
            U64(identity, participant.ObjectGeneration);
            U64(identity, participant.AuthorityOwnerGeneration);
            U16(stream, checked((ushort)identity.Length));
            identity.Position = 0;
            identity.CopyTo(stream);
        }
        I64(stream, applicationVersion);

        U32(stream, 1);
        U64(stream, 1);
        stream.WriteByte(participant.RecoveryPayload.IsEmpty ? (byte)1 : (byte)2);
        using (var state = new MemoryStream())
        {
            U64(state, checked((ulong)participant.ApplicationState.Length));
            state.Write(participant.ApplicationState.Span);
            if (!participant.RecoveryPayload.IsEmpty)
            {
                U64(state, checked((ulong)participant.RecoveryPayload.Length));
                state.Write(participant.RecoveryPayload.Span);
            }
            U64(stream, checked((ulong)state.Length));
            state.Position = 0;
            state.CopyTo(stream);
        }

        U32(stream, 1);
        U64(stream, 1);
        U64(stream, acceptedBoundary);
        U64(stream, 0);

        U32(stream, checked((uint)accepted.Length));
        foreach (var job in accepted)
        {
            if (!ZLinkRelocationEnvelopeCodec.TryValidateCanonicalFrozenRecord(
                    job.Payload.Span))
                throw new ZLinkRelocationDataLostException(
                    "Standalone Actor accepted journal contains a malformed frozen record.");
            U64(stream, 1);
            U64(stream, job.AcceptedSequence);
            stream.Write(job.Payload.Span);
        }
        U32(stream, 0); // logical timers
        U32(stream, 0); // pending timer ticks
        U32(stream, 0); // terminal completions

        stream.Position = 0;
        var canonical = ZLinkRelocationEnvelopeCodec.Decode(
            stream,
            inventory.InventoryDigest);
        var projected = canonical.Participants.Single() with
        {
            AuthorityKey = participant.AuthorityKey,
            RecoveryPayload = participant.RecoveryPayload,
            CompletionPayload = participant.CompletionPayload
        };
        return inventory with
        {
            Participants = [projected],
            CanonicalLogicalStream = canonical.CanonicalLogicalStream,
            CanonicalLayout = canonical.CanonicalLayout,
            CanonicalRelocationHigh = canonical.CanonicalRelocationHigh,
            CanonicalRelocationLow = canonical.CanonicalRelocationLow,
            CanonicalApplicationVersion = canonical.CanonicalApplicationVersion
        };
    }

    private static void Text8(Stream stream, string value)
    {
        var bytes = StrictUtf8.GetBytes(value);
        if (bytes.Length is < 1 or > byte.MaxValue || bytes.AsSpan().Contains((byte)0))
            throw new ArgumentOutOfRangeException(nameof(value));
        stream.WriteByte(checked((byte)bytes.Length));
        stream.Write(bytes);
    }

    private static void U16(Stream stream, ushort value)
    {
        Span<byte> bytes = stackalloc byte[sizeof(ushort)];
        BinaryPrimitives.WriteUInt16BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private static void U32(Stream stream, uint value)
    {
        Span<byte> bytes = stackalloc byte[sizeof(uint)];
        BinaryPrimitives.WriteUInt32BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private static void U64(Stream stream, ulong value)
    {
        Span<byte> bytes = stackalloc byte[sizeof(ulong)];
        BinaryPrimitives.WriteUInt64BigEndian(bytes, value);
        stream.Write(bytes);
    }

    private static void I64(Stream stream, long value) =>
        U64(stream, checked((ulong)value));
}
