using System.Buffers.Binary;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Actors;

/// <summary>
/// Projects the standalone Actor inventory into the canonical relocation root.
/// Saved-work records remain byte-exact in their participant order.
/// </summary>
internal static class ZLinkCanonicalActorRelocationWriter
{
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
        if (!ZLinkActorAuthorityPayloadCodec.TryGetActorId(
                participant.AuthorityKey, out var actorId))
            throw new ZLinkRelocationDataLostException(
                "Canonical standalone Actor relocation has an invalid authority key.");

        var accepted = participant.AcceptedJobs
            .OrderBy(static job => job.AcceptedSequence)
            .ToList();
        // Older in-process roots carried the routed-join recovery in ZLRP.
        // The frozen kind-1 body cannot carry that private field, so project a
        // valid record into saved-work before serialization. New producers
        // already do this in CreateImmutableRoot and are left byte-for-byte
        // unchanged.
        if (!participant.RecoveryPayload.IsEmpty
            && !accepted.Any(static job =>
                ZLinkActorRemoteJoinRecoverySavedWork.TryDecode(
                    job.Payload.Span, out _, out _))
            && TryReadLegacyRemoteJoinRecovery(
                participant.RecoveryPayload.Span, out var source, out var remote))
        {
            accepted.Add(ZLinkActorRemoteJoinRecoverySavedWork.Create(
                accepted.Count == 0
                    ? 1UL
                    : checked(accepted[^1].AcceptedSequence + 1),
                source,
                remote));
        }
        var savedWork = new List<ServiceWirePilotCodec.RelocationSavedWork>(
            accepted.Count);
        foreach (var job in accepted)
        {
            if (!ZLinkRelocationEnvelopeCodec.TryValidateCanonicalFrozenRecord(
                    job.Payload.Span))
                throw new ZLinkRelocationDataLostException(
                    "Standalone Actor saved work contains a malformed frozen record.");
            savedWork.Add(new ServiceWirePilotCodec.RelocationSavedWork(
                ParticipantId: 1,
                job.AcceptedSequence,
                job.Payload.ToArray()));
        }
        var aggregateId = inventory.AggregateId.ToByteArray(bigEndian: true);
        var encoded = ServiceWirePilotCodec.EncodeRelocationEnvelopeV1(new(
            BinaryPrimitives.ReadUInt64BigEndian(aggregateId.AsSpan(0, 8)),
            BinaryPrimitives.ReadUInt64BigEndian(aggregateId.AsSpan(8, 8)),
            new ServiceWirePilotCodec.RelocationActorIdentity(
                actorId,
                participant.ObjectGeneration,
                participant.AuthorityOwnerGeneration),
            applicationVersion,
            [new ServiceWirePilotCodec.RelocationApplicationState(
                ParticipantId: 1,
                HasState: true,
                participant.ApplicationState.ToArray())],
            savedWork,
            [],
            []));
        using var stream = new MemoryStream(encoded, writable: false);
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
            CanonicalRelocationHigh = canonical.CanonicalRelocationHigh,
            CanonicalRelocationLow = canonical.CanonicalRelocationLow,
            CanonicalApplicationVersion = canonical.CanonicalApplicationVersion
        };
    }

    private static bool TryReadLegacyRemoteJoinRecovery(
        ReadOnlySpan<byte> encoded,
        out ZLinkActorRelocationSourceFence source,
        out ReadOnlyMemory<byte> remote)
    {
        source = default!;
        remote = default;
        try
        {
            var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(encoded);
            source = ZLinkActorRelocationSourceFenceCodec.Decode(
                recovery.MembershipMutation.Span);
            if (recovery.OperationRecovery.IsEmpty
                && source.LegacyRemoteJoinRecovery.IsEmpty)
                return false;
            remote = ZLinkActorRemoteJoinRecoveryCodec.Encode(
                ZLinkActorRemoteJoinRecoveryCodec.Decode(
                    recovery.OperationRecovery.Span,
                    source.LegacyRemoteJoinRecovery.Span));
            return true;
        }
        catch (Exception error) when (error is InvalidDataException
                                      or EndOfStreamException
                                      or ArgumentException
                                      or OverflowException)
        {
            return false;
        }
    }
}
