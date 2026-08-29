using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Actors;

internal sealed record ZLinkStandaloneActorRelocationTargetFence(
    Guid RelocationId,
    ulong TargetAttemptGeneration,
    RoutingId NodeRid,
    ulong NodeGeneration,
    ZLinkLocationOwnerToken Owner);

internal enum ZLinkStandaloneActorCanonicalPhase : byte
{
    Committed = 4,
    Activating = 5,
    Activated = 6,
    Cleaning = 7,
    Completed = 8
}

internal sealed record ZLinkStandaloneActorRelocationProgress(
    ZLinkAuthoritySnapshot Authority,
    ZLinkActorRelocationAuthorityPayload Phase,
    ZLinkRelocationEnvelope Root)
{
    internal ZLinkCanonicalRelocationAuthorityProjection? Canonical { get; init; }
}

/// <summary>
/// Reads and advances the target-owned standalone Actor relocation phase.
/// </summary>
internal sealed class ZLinkStandaloneActorRelocationProgressCoordinator(
    IZLinkLocationRepository authorityStore,
    IZLinkRelocationRepository relocationStore,
    ZLinkStandaloneActorRelocationTargetFence? targetFence = null)
{
    private const int MaxConflictRetries = 8;

    internal ValueTask<ZLinkStandaloneActorRelocationProgress> ReadAsync(
        ZLinkRelocationEnvelope identity,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken) =>
        ReadCurrentAsync(identity, targetOwner, cancellationToken);

    internal async ValueTask<ZLinkStandaloneActorRelocationProgress>
        AdvanceCanonicalPhaseAsync(
            ZLinkRelocationEnvelope identity,
            ZLinkStandaloneActorCanonicalPhase expected,
            ZLinkStandaloneActorCanonicalPhase next,
            ZLinkLocationOwnerToken targetOwner,
            CancellationToken cancellationToken)
    {
        if ((byte)expected < (byte)ZLinkStandaloneActorCanonicalPhase.Committed
            || (byte)next > (byte)ZLinkStandaloneActorCanonicalPhase.Activated
            || (byte)next != (byte)expected + 1)
            throw new ArgumentOutOfRangeException(
                nameof(next),
                "Canonical standalone Actor phases must advance by one durable boundary.");
        for (var attempt = 0; attempt < MaxConflictRetries; attempt++)
        {
            var current = await ReadCurrentAsync(
                    identity,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
            var canonical = current.Canonical
                            ?? throw new ZLinkRelocationDataLostException(
                                "Standalone Actor canonical phase authority is unavailable.");
            if (canonical.Phase == (byte)next) return current;
            if (canonical.Phase != (byte)expected)
                throw new ZLinkRelocationDataLostException(
                    $"Canonical standalone Actor relocation phase is '{canonical.Phase}', not '{(byte)expected}'.");
            var payload = ZLinkCanonicalRelocationAuthorityStateCodec
                .ReplaceRelocationState(
                    current.Authority.Payload.Span,
                    canonical.State with { Phase = (byte)next },
                    current.Root);
            var exchanged = await authorityStore.CompareExchangeAuthorityAsync(
                    identity.Participants.Single().AuthorityKey,
                    current.Authority.StoreVersion,
                    new ZLinkAuthorityMutation.Put(
                        payload,
                        ZLinkAuthorityGenerationTransition.Preserve,
                        null,
                        null),
                    cancellationToken)
                .ConfigureAwait(false);
            if (exchanged is ZLinkAuthorityCompareExchangeResult.Stored)
                continue;
            if (exchanged is not ZLinkAuthorityCompareExchangeResult.Conflict)
                throw new InvalidOperationException(
                    "Authority Store rejected canonical standalone Actor phase progress.");
        }
        throw Moving(
            "canonical phase advance conflicted after the bounded retry limit");
    }

    internal async ValueTask NormalizeSteadyAsync(
        ZLinkRelocationEnvelope identity,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken)
    {
        for (var attempt = 0; attempt < MaxConflictRetries; attempt++)
        {
            var current = await ReadCurrentAsync(
                    identity,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
            var phaseReady = current.Canonical is { } canonicalPhase
                ? canonicalPhase.Phase is >=
                    (byte)ZLinkStandaloneActorCanonicalPhase.Activated and <=
                    (byte)ZLinkStandaloneActorCanonicalPhase.Completed
                : current.Phase.Phase is
                    ZLinkActorRelocationAuthorityPhase.Activated
                    or ZLinkActorRelocationAuthorityPhase.Cleaning
                    or ZLinkActorRelocationAuthorityPhase.Completed;
            if (!phaseReady)
                throw new ZLinkRelocationDataLostException(
                    "Standalone Actor relocation is not ready for steady normalization.");
            var payload = current.Canonical is { } canonical
                ? canonical.SteadyAuthorityPayload
                : LegacySteadyPayload(current.Authority.Payload, current.Phase);
            // Direct transfer (spec 52 §3.1.7) carries the immutable root on
            // command 52 rather than through the Relocation Store.  Its
            // canonical authority has no tree pointer to load or delete.
            if (current.Canonical is { } directCanonical
                && ZLinkStandaloneActorRelocationPrecommitCoordinator
                    .IsDirectTransferReference(
                        directCanonical.RelocationReference,
                        directCanonical.RelocationChecksumCrc32c))
            {
                var direct = await authorityStore.CompareExchangeAuthorityAsync(
                        identity.Participants.Single().AuthorityKey,
                        current.Authority.StoreVersion,
                        new ZLinkAuthorityMutation.Put(
                            payload,
                            ZLinkAuthorityGenerationTransition.Preserve,
                            null,
                            null),
                        cancellationToken)
                    .ConfigureAwait(false);
                if (direct is ZLinkAuthorityCompareExchangeResult.Stored)
                    return;
                if (direct is ZLinkAuthorityCompareExchangeResult.Conflict)
                    continue;
                throw new InvalidOperationException(
                    "Authority Store rejected direct standalone Actor steady normalization.");
            }
            if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                    current.Authority.Payload.Span,
                    out var publication))
                throw new ZLinkRelocationDataLostException(
                    "Standalone Actor relocation root pointer disappeared during normalization.");
            var relocationReference = current.Canonical?.RelocationReference
                                      ?? publication.Reference;
            var exchanged = await authorityStore.CompareExchangeAuthorityAsync(
                    identity.Participants.Single().AuthorityKey,
                    current.Authority.StoreVersion,
                    new ZLinkAuthorityMutation.Put(
                        payload,
                        ZLinkAuthorityGenerationTransition.Preserve,
                        null,
                        null),
                    cancellationToken)
                .ConfigureAwait(false);
            if (exchanged is ZLinkAuthorityCompareExchangeResult.Stored)
            {
                try
                {
                    await ZLinkRelocationTreeStore.DeleteTreeAsync(
                            relocationStore,
                            relocationReference,
                            CancellationToken.None)
                        .ConfigureAwait(false);
                }
                catch
                {
                    // Authority no longer references the immutable root. Its
                    // retention policy can finish cleanup after a store error.
                }
                return;
            }
            if (exchanged is not ZLinkAuthorityCompareExchangeResult.Conflict)
                throw new InvalidOperationException(
                    "Authority Store rejected standalone Actor steady normalization.");
        }
        throw Moving("steady normalization conflicted after the bounded retry limit");
    }

    private async ValueTask<ZLinkStandaloneActorRelocationProgress> ReadCurrentAsync(
        ZLinkRelocationEnvelope identity,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken)
    {
        var participant = identity.Participants.Single();
        var read = await authorityStore.ReadAuthorityAsync(
                participant.AuthorityKey,
                cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found
            || found.Snapshot.OwnerId != targetOwner.OwnerId
            || found.Snapshot.OwnerLeaseGeneration != targetOwner.LeaseGeneration)
            throw new ZLinkRelocationDataLostException(
                "Standalone Actor relocation authority changed during replay.");
        if (ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                found.Snapshot.Payload.Span, out var directCanonical)
            && ZLinkStandaloneActorRelocationPrecommitCoordinator
                .IsDirectTransferReference(
                    directCanonical.RelocationReference,
                    directCanonical.RelocationChecksumCrc32c))
        {
            if (directCanonical.RelocationHigh != identity.CanonicalRelocationHigh
                || directCanonical.RelocationLow != identity.CanonicalRelocationLow)
                throw new ZLinkRelocationDataLostException(
                    "Standalone Actor direct-transfer authority changed its relocation identity.");
            ValidateCanonicalFence(found.Snapshot, directCanonical);
            return new ZLinkStandaloneActorRelocationProgress(
                found.Snapshot,
                CanonicalPhase(identity, directCanonical),
                identity)
            {
                Canonical = directCanonical
            };
        }
        if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span, out var publication)
            || publication.AggregateId != identity.AggregateId)
            throw new ZLinkRelocationDataLostException(
                "Standalone Actor relocation authority changed during replay.");
        var root = await ZLinkRelocationTreeStore.GetAsync(
                relocationStore,
                publication.Reference,
                publication.ChecksumCrc32c,
                cancellationToken)
            .ConfigureAwait(false);
        if (root.AggregateId != identity.AggregateId
            || root.CanonicalRelocationHigh != identity.CanonicalRelocationHigh
            || root.CanonicalRelocationLow != identity.CanonicalRelocationLow
            || root.CanonicalLogicalStream.IsEmpty
            && root.AggregateGeneration != 0
            && root.AggregateGeneration != publication.AggregateGeneration)
            throw new ZLinkRelocationDataLostException(
                "Standalone Actor replay root does not match its authority "
                + $"(root={root.AggregateGeneration}, "
                + $"authority={publication.AggregateGeneration}, "
                + $"expected={identity.AggregateGeneration}).");
        root = root with
        {
            AggregateGeneration = publication.AggregateGeneration
        };
        ZLinkActorRelocationAuthorityPayload phase;
        ZLinkCanonicalRelocationAuthorityProjection? canonical = null;
        if (publication.IsCanonical)
        {
            if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    found.Snapshot.Payload.Span,
                    out canonical)
                || canonical.RelocationReference != publication.Reference
                || canonical.RelocationChecksumCrc32c != publication.ChecksumCrc32c
                || !ZLinkActorAuthorityPayloadCodec.TryDecode(
                    canonical.SteadyAuthorityPayload.Span,
                    out _))
                throw new ZLinkRelocationDataLostException(
                    "Standalone Actor canonical authority or target phase seed is invalid.");
            ValidateCanonicalFence(found.Snapshot, canonical);
            phase = CanonicalPhase(identity, canonical);
        }
        else if (!ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
                     publication.ApplicationPayload.Span,
                     out phase)
                 || phase.RelocationId != identity.AggregateId)
            throw new ZLinkRelocationDataLostException(
                "Standalone Actor legacy relocation phase is invalid.");
        return new ZLinkStandaloneActorRelocationProgress(
            found.Snapshot,
            phase,
            root)
        {
            Canonical = canonical
        };
    }

    private void ValidateCanonicalFence(
        ZLinkAuthoritySnapshot snapshot,
        ZLinkCanonicalRelocationAuthorityProjection canonical)
    {
        var expected = targetFence
                       ?? throw new InvalidOperationException(
                           "Canonical standalone Actor progress requires an exact target fence.");
        Span<byte> id = stackalloc byte[16];
        expected.RelocationId.TryWriteBytes(id, bigEndian: true, out _);
        if (canonical.RelocationHigh
            != System.Buffers.Binary.BinaryPrimitives.ReadUInt64BigEndian(id)
            || canonical.RelocationLow
            != System.Buffers.Binary.BinaryPrimitives.ReadUInt64BigEndian(id[8..])
            || canonical.TargetAttemptGeneration
            != expected.TargetAttemptGeneration
            || !StringComparer.Ordinal.Equals(
                canonical.State.TargetNodeRid,
                expected.NodeRid.ToHex())
            || canonical.State.TargetNodeGeneration != expected.NodeGeneration
            || !StringComparer.Ordinal.Equals(
                canonical.TargetOwnerId,
                expected.Owner.OwnerId)
            || canonical.TargetOwnerLeaseGeneration
            != checked((ulong)expected.Owner.LeaseGeneration)
            || !StringComparer.Ordinal.Equals(
                snapshot.OwnerId,
                expected.Owner.OwnerId)
            || snapshot.OwnerLeaseGeneration != expected.Owner.LeaseGeneration)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "Standalone Actor relocation target attempt fence changed.",
                retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);
    }

    private static ZLinkActorRelocationAuthorityPayload CanonicalPhase(
        ZLinkRelocationEnvelope identity,
        ZLinkCanonicalRelocationAuthorityProjection canonical)
    {
        if (!ZLinkActorAuthorityPayloadCodec.TryDecode(
                canonical.SteadyAuthorityPayload.Span,
                out var authority))
            throw new ZLinkRelocationDataLostException(
                "Standalone Actor canonical steady authority is invalid.");
        return new ZLinkActorRelocationAuthorityPayload(
            identity.AggregateId,
            CanonicalActorPhase(canonical.Phase),
            default,
            ZLinkActorAuthorityPayloadCodec.Encode(authority));
    }

    private static ZLinkActorRelocationAuthorityPhase CanonicalActorPhase(
        byte phase) => phase switch
        {
            4 or 5 or 6 => ZLinkActorRelocationAuthorityPhase.Activated,
            7 => ZLinkActorRelocationAuthorityPhase.Cleaning,
            8 => ZLinkActorRelocationAuthorityPhase.Completed,
            _ => throw new ZLinkRelocationDataLostException(
                $"Canonical standalone Actor phase '{phase}' is not target-recoverable.")
        };

    private static ReadOnlyMemory<byte> LegacySteadyPayload(
        ReadOnlyMemory<byte> current,
        ZLinkActorRelocationAuthorityPayload phase)
    {
        if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                current.Span,
                out var publication))
            throw new ZLinkRelocationDataLostException(
                "Legacy standalone Actor relocation publication disappeared.");
        return ZLinkRelocationAuthorityPayloadCodec.Encode(
            publication with
            {
                ApplicationPayload = ZLinkActorRelocationAuthorityPayloadCodec.Encode(
                    phase with { Phase = ZLinkActorRelocationAuthorityPhase.Steady })
            });
    }

    private static ZLinkFrameworkException Moving(string reason) => new(
        ZLinkFrameworkErrorKind.Unavailable,
        $"Standalone Actor relocation {reason}.",
        retryAdvice: ZLinkRetryAdvice.RetryAfterBackoff);

}
