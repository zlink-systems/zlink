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
/// Publishes standalone Actor replay progress by storing a new immutable root
/// and replacing its Location Store reference and terminal counts in one CAS.
/// </summary>
internal sealed class ZLinkStandaloneActorRelocationProgressCoordinator(
    IZLinkLocationRepository authorityStore,
    IZLinkRelocationRepository relocationStore,
    ZLinkStandaloneActorRelocationTargetFence? targetFence = null)
{
    private const int MaxConflictRetries = 8;
    private static readonly TimeSpan Retention = TimeSpan.FromHours(24);

    internal ValueTask<ZLinkStandaloneActorRelocationProgress> ReadAsync(
        ZLinkRelocationEnvelope identity,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken) =>
        ReadCurrentAsync(identity, targetOwner, cancellationToken);

    internal ValueTask<ZLinkStandaloneActorRelocationProgress> AdvanceReplayAsync(
        ZLinkRelocationEnvelope identity,
        ulong acceptedSequence,
        ZLinkCanonicalTerminalCompletion? completion,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken) =>
        UpdateAsync(
            identity,
            current =>
            {
                var participant = current.Participants.Single();
                if (acceptedSequence != 0
                    && participant.ReplayCursor == acceptedSequence)
                    return current;
                if (participant.ReplayCursor == ulong.MaxValue
                    || acceptedSequence != participant.ReplayCursor + 1)
                    throw new ZLinkRelocationDataLostException(
                        $"Standalone Actor replay sequence '{acceptedSequence}' does not follow cursor '{participant.ReplayCursor}'.");
                var successor = current;
                if (completion is not null
                    && !participant.TerminalCompletions.Any(candidate =>
                        SameCompletion(candidate, completion)))
                    successor = ZLinkRelocationEnvelopeCodec
                        .AppendCanonicalTerminalCompletion(successor, completion);
                return ZLinkRelocationEnvelopeCodec.AdvanceCanonicalReplayCursor(
                    successor,
                    participant.CanonicalParticipantId,
                    acceptedSequence);
            },
            targetOwner,
            cancellationToken);

    internal ValueTask<ZLinkStandaloneActorRelocationProgress> CompleteReplyAsync(
        ZLinkRelocationEnvelope identity,
        ZLinkCanonicalTerminalCompletion completion,
        byte deliveryState,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken) =>
        UpdateAsync(
            identity,
            current => ZLinkRelocationEnvelopeCodec
                .CompleteCanonicalTerminalDelivery(
                    current,
                    completion.OperationHigh,
                    completion.OperationLow,
                    completion.SourceOwnerId,
                    completion.SourceOwnerLeaseGeneration,
                    completion.SourceNodeRid,
                    completion.SourceNodeGeneration,
                    deliveryState),
            targetOwner,
            cancellationToken);

    internal async ValueTask<ZLinkStandaloneActorRelocationProgress>
        AdvancePhaseAsync(
        ZLinkRelocationEnvelope identity,
        ZLinkActorRelocationAuthorityPhase expected,
        ZLinkActorRelocationAuthorityPhase next,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken)
    {
        if (IsCanonicalRoot(identity))
        {
            var canonical = (expected, next) switch
            {
                (ZLinkActorRelocationAuthorityPhase.Activated,
                    ZLinkActorRelocationAuthorityPhase.Cleaning) =>
                    (ZLinkStandaloneActorCanonicalPhase.Activated,
                        ZLinkStandaloneActorCanonicalPhase.Cleaning),
                (ZLinkActorRelocationAuthorityPhase.Cleaning,
                    ZLinkActorRelocationAuthorityPhase.Completed) =>
                    (ZLinkStandaloneActorCanonicalPhase.Cleaning,
                        ZLinkStandaloneActorCanonicalPhase.Completed),
                _ => throw new InvalidOperationException(
                    "Canonical standalone Actor phase transition is invalid.")
            };
            return await AdvanceCanonicalPhaseAsync(
                    identity,
                    canonical.Item1,
                    canonical.Item2,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        for (var attempt = 0; attempt < MaxConflictRetries; attempt++)
        {
            var current = await ReadCurrentAsync(
                    identity,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
            if (current.Phase.Phase == next) return current;
            if (current.Phase.Phase != expected)
                throw new ZLinkRelocationDataLostException(
                    $"Standalone Actor relocation phase is '{current.Phase.Phase}', not '{expected}'.");
            if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                    current.Authority.Payload.Span,
                    out var publication))
                throw new ZLinkRelocationDataLostException(
                    "Standalone Actor relocation publication disappeared during phase advance.");
            var payload = ZLinkRelocationAuthorityPayloadCodec.Encode(
                publication with
                {
                    ApplicationPayload = ZLinkActorRelocationAuthorityPayloadCodec
                        .Encode(current.Phase with { Phase = next })
                });
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
                    "Authority Store rejected standalone Actor phase progress.");
        }
        throw Moving("phase advance conflicted after the bounded retry limit");
    }

    private static bool IsCanonicalRoot(
        ZLinkRelocationEnvelope envelope)
    {
        try
        {
            _ = ZLinkCanonicalParticipantRecoveryCodec.Decode(
                envelope.Participants.Single().RecoveryPayload.Span);
            return true;
        }
        catch (Exception error) when (error is InvalidDataException
                                      or EndOfStreamException)
        {
            return false;
        }
    }

    internal async ValueTask<ZLinkStandaloneActorRelocationProgress>
        AdvanceCanonicalPhaseAsync(
            ZLinkRelocationEnvelope identity,
            ZLinkStandaloneActorCanonicalPhase expected,
            ZLinkStandaloneActorCanonicalPhase next,
            ZLinkLocationOwnerToken targetOwner,
            CancellationToken cancellationToken)
    {
        if ((byte)expected < (byte)ZLinkStandaloneActorCanonicalPhase.Committed
            || (byte)next > (byte)ZLinkStandaloneActorCanonicalPhase.Completed
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
            if (current.Phase.Phase != ZLinkActorRelocationAuthorityPhase.Completed
                || current.Phase.TerminalCompletionCount
                   != current.Phase.AcceptedRequestCount
                || current.Phase.PendingRelayCount != 0)
                throw new ZLinkRelocationDataLostException(
                    "Standalone Actor relocation is not ready for steady normalization.");
            var payload = current.Canonical is { } canonical
                ? canonical.SteadyAuthorityPayload
                : LegacySteadyPayload(current.Authority.Payload, current.Phase);
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

    internal async ValueTask<ZLinkStandaloneActorRelocationProgress>
        PublishAdmissionReadyAuthorityAsync(
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
            var canonical = current.Canonical
                            ?? throw new ZLinkRelocationDataLostException(
                                "Standalone Actor source cleanup requires canonical authority.");
            if (canonical.SourceCleanupState == 1) return current;
            if (canonical.SourceCleanupState != 0
                || current.Phase.Phase
                != ZLinkActorRelocationAuthorityPhase.Cleaning)
                throw new ZLinkRelocationDataLostException(
                    "Standalone Actor source cleanup phase is not recoverable.");
            var payload = ZLinkCanonicalRelocationAuthorityStateCodec
                .ReplaceRelocationState(
                    current.Authority.Payload.Span,
                    canonical.State with { SourceCleanupState = 1 },
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
                    "Authority Store rejected standalone Actor source cleanup.");
        }
        throw Moving("source cleanup conflicted after the bounded retry limit");
    }

    private async ValueTask<ZLinkStandaloneActorRelocationProgress> UpdateAsync(
        ZLinkRelocationEnvelope identity,
        Func<ZLinkRelocationEnvelope, ZLinkRelocationEnvelope> update,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken)
    {
        if (identity.CanonicalLogicalStream.IsEmpty)
            throw new ArgumentException(
                "Standalone Actor replay requires a canonical relocation root.",
                nameof(identity));
        for (var attempt = 0; attempt < MaxConflictRetries; attempt++)
        {
            var current = await ReadCurrentAsync(
                    identity,
                    targetOwner,
                    cancellationToken)
                .ConfigureAwait(false);
            var successor = update(current.Root);
            if (successor.CanonicalLogicalStream.Span.SequenceEqual(
                    current.Root.CanonicalLogicalStream.Span))
                return current;

            var stored = await ZLinkRelocationTreeStore.PutAsync(
                    relocationStore,
                    successor,
                    Retention,
                    cancellationToken)
                .ConfigureAwait(false);
            var terminalCount = checked((uint)successor.Participants.Sum(
                static participant => participant.TerminalCompletions.Count));
            var pendingCount = checked((uint)successor.Participants.Sum(
                static participant => participant.PendingRelayCount));
            var nextPhase = current.Phase with
            {
                TerminalCompletionCount = terminalCount,
                PendingRelayCount = pendingCount
            };
            if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                    current.Authority.Payload.Span,
                    out var publication))
                throw new ZLinkRelocationDataLostException(
                    "Standalone Actor relocation publication disappeared during replay.");
            var nextPayload = current.Canonical is { } canonical
                ? ZLinkCanonicalRelocationAuthorityStateCodec
                    .ReplaceRelocationState(
                        current.Authority.Payload.Span,
                        canonical.State with
                        {
                            RelocationReference = stored.Root.Reference,
                            RelocationChecksumCrc32c = stored.Root.ChecksumCrc32c
                        },
                        successor)
                : ZLinkRelocationAuthorityPayloadCodec.Encode(
                    publication with
                    {
                        Reference = stored.Root.Reference,
                        ChecksumCrc32c = stored.Root.ChecksumCrc32c,
                        ApplicationPayload = ZLinkActorRelocationAuthorityPayloadCodec
                            .Encode(nextPhase)
                    });
            ZLinkAuthorityCompareExchangeResult result;
            try
            {
                result = await authorityStore.CompareExchangeAuthorityAsync(
                        successor.Participants.Single().AuthorityKey,
                        current.Authority.StoreVersion,
                        new ZLinkAuthorityMutation.Put(
                            nextPayload,
                            ZLinkAuthorityGenerationTransition.Preserve,
                            null,
                            null),
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch
            {
                var reconciled = await TryReadPublishedSuccessorAsync(
                        identity,
                        successor,
                        stored.Root,
                        targetOwner)
                    .ConfigureAwait(false);
                if (reconciled is not null) return reconciled;

                // A failed response does not prove that this content-addressed
                // root is unused. Another writer may have published the same
                // reference, so retention owns any later orphan cleanup.
                throw;
            }
            if (result is ZLinkAuthorityCompareExchangeResult.Stored success)
            {
                // Command 35 retries prove that a published progress root is a
                // monotonic successor of the sealed root. Keep predecessor
                // roots until retention expires so a restarted target can
                // verify that chain after the authority pointer advances.
                return new ZLinkStandaloneActorRelocationProgress(
                    success.Snapshot,
                    nextPhase,
                    successor)
                {
                    Canonical = current.Canonical is null
                        ? null
                        : ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                            success.Snapshot.Payload.Span,
                            out var advancedCanonical)
                            ? advancedCanonical
                            : throw new ZLinkRelocationDataLostException(
                                "Standalone Actor replay regressed from canonical authority.")
                };
            }
            var published = await TryReadPublishedSuccessorAsync(
                    identity,
                    successor,
                    stored.Root,
                    targetOwner)
                .ConfigureAwait(false);
            if (published is not null) return published;

            // The CAS result alone cannot prove that this content-addressed
            // root is an orphan: another concurrent writer can still publish
            // the same reference. Leave it to retention instead of deleting a
            // root that may become authoritative.
            if (result is not ZLinkAuthorityCompareExchangeResult.Conflict)
                throw new InvalidOperationException(
                    "Authority Store rejected standalone Actor replay progress.");
        }
        throw Moving("replay progress conflicted after the bounded retry limit");
    }

    private async ValueTask<ZLinkStandaloneActorRelocationProgress?>
        TryReadPublishedSuccessorAsync(
        ZLinkRelocationEnvelope identity,
        ZLinkRelocationEnvelope successor,
        ZLinkRelocationStored stored,
        ZLinkLocationOwnerToken targetOwner)
    {
        try
        {
            var current = await ReadCurrentAsync(
                    identity,
                    targetOwner,
                    CancellationToken.None)
                .ConfigureAwait(false);
            if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                    current.Authority.Payload.Span,
                    out var publication)
                || !string.Equals(
                    publication.Reference,
                    stored.Reference,
                    StringComparison.Ordinal)
                || publication.ChecksumCrc32c != stored.ChecksumCrc32c
                || !current.Root.CanonicalLogicalStream.Span.SequenceEqual(
                    successor.CanonicalLogicalStream.Span))
                return null;
            return current;
        }
        catch
        {
            // Reconciliation is best-effort. If the exact authority cannot be
            // observed, the caller retains the original CAS outcome and the
            // immutable root remains protected by retention.
            return null;
        }
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
            || found.Snapshot.OwnerLeaseGeneration != targetOwner.LeaseGeneration
            || !ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span,
                out var publication)
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
            || root.CanonicalRelocationLow != identity.CanonicalRelocationLow)
            throw new ZLinkRelocationDataLostException(
                "Standalone Actor replay root does not match its authority.");
        var terminalCount = checked((uint)root.Participants.Sum(
            static item => item.TerminalCompletions.Count));
        var pendingCount = checked((uint)root.Participants.Sum(
            static item => item.PendingRelayCount));
        ZLinkActorRelocationAuthorityPayload phase;
        ZLinkCanonicalRelocationAuthorityProjection? canonical = null;
        if (publication.IsCanonical)
        {
            if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    found.Snapshot.Payload.Span,
                    out canonical)
                || canonical.RelocationReference != publication.Reference
                || canonical.RelocationChecksumCrc32c != publication.ChecksumCrc32c
                || !ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
                    ZLinkCanonicalParticipantRecoveryCodec.Decode(
                            root.Participants.Single().RecoveryPayload.Span)
                        .AuthorityPayload.Span,
                    out var rootPhase))
                throw new ZLinkRelocationDataLostException(
                    "Standalone Actor canonical authority or immutable phase seed is invalid.");
            ValidateCanonicalFence(found.Snapshot, canonical);
            phase = rootPhase with
            {
                Phase = CanonicalActorPhase(canonical.Phase),
                TerminalCompletionCount = canonical.TerminalCompletionCount,
                PendingRelayCount = canonical.PendingRelayCount
            };
        }
        else if (!ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
                     publication.ApplicationPayload.Span,
                     out phase)
                 || phase.RelocationId != identity.AggregateId)
            throw new ZLinkRelocationDataLostException(
                "Standalone Actor legacy relocation phase is invalid.");
        if (phase.TerminalCompletionCount != terminalCount
            || phase.PendingRelayCount != pendingCount)
            throw new ZLinkRelocationDataLostException(
                "Standalone Actor authority terminal counts do not match its root.");
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

    private static bool SameCompletion(
        ZLinkCanonicalTerminalCompletion left,
        ZLinkCanonicalTerminalCompletion right) =>
        left.OperationHigh == right.OperationHigh
        && left.OperationLow == right.OperationLow
        && left.SourceOwnerId == right.SourceOwnerId
        && left.SourceOwnerLeaseGeneration == right.SourceOwnerLeaseGeneration
        && left.SourceNodeRid == right.SourceNodeRid
        && left.SourceNodeGeneration == right.SourceNodeGeneration;
}
