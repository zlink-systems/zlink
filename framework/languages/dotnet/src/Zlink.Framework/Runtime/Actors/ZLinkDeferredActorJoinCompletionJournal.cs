using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Actors;

internal enum ZLinkDeferredJoinCompletionCursor : byte
{
    Prepared = 1,
    Committed = 2,
    Delivered = 3
}

internal sealed record ZLinkDeferredJoinCompletionRecord(
    string ActorId,
    ulong ObjectGeneration,
    ZLinkActorJoinOperationId OperationId,
    ActorRef Actor,
    string? ReplyContentType,
    ReadOnlyMemory<byte> Reply,
    ZLinkDeferredJoinCompletionCursor Cursor);

internal sealed record ZLinkDeferredJoinCompletionRoot(
    ZLinkAuthorityKey AuthorityKey,
    string Reference,
    uint ChecksumCrc32c,
    string ExpectedStoreVersion,
    ZLinkRelocationEnvelope Envelope,
    ZLinkDeferredJoinCompletionRecord Completion);

/// <summary>
/// Persists the post-commit Actor Join completion in the relocation root that
/// the Actor authority publishes. Updating a cursor always writes a new
/// immutable root before the authority CAS; the old root is removed only after
/// the CAS stops referencing it.
/// </summary>
internal sealed class ZLinkDeferredActorJoinCompletionJournal(
    IZLinkLocationRepository authorityStore,
    IZLinkRelocationRepository relocationStore)
{
    private static readonly TimeSpan Retention = TimeSpan.FromHours(24);
    private readonly ConcurrentDictionary<
        (string Reference, uint Checksum),
        Lazy<Task<ZLinkRelocationEnvelope>>> _rootReads = new();

    internal async ValueTask<ZLinkDeferredJoinCompletionRoot> PrepareAsync(
        string actorId,
        ZLinkActorJoinOperationId operationId,
        ActorRef actor,
        string? replyContentType,
        ReadOnlyMemory<byte> reply,
        CancellationToken cancellationToken)
    {
        ValidateIdentity(actorId, operationId, actor);
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var read = await authorityStore.ReadAuthorityAsync(key, cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Actor '{actorId}' does not have authority for durable Join completion.");

        var snapshot = found.Snapshot;
        if (snapshot.ObjectGeneration != actor.ObjectGeneration)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actorId}' authority generation changed before Join completion was prepared.");

        if (await TryReadPublishedAsync(key, snapshot, cancellationToken).ConfigureAwait(false)
            is { } existing)
        {
            EnsureSameOperation(existing.Completion, operationId, actor);
            return existing;
        }

        if (!ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                snapshot.Payload.Span,
                out _))
            throw new InvalidDataException(
                $"Actor '{actorId}' authority payload cannot host a relocation manifest.");

        var completion = new ZLinkDeferredJoinCompletionRecord(
            actorId,
            actor.ObjectGeneration,
            operationId,
            actor,
            replyContentType,
            reply.ToArray(),
            ZLinkDeferredJoinCompletionCursor.Prepared);
        var replacedReference = default(string);
        var replacedRootIsActorOnly = false;
        var applicationAuthorityPayload = snapshot.Payload;
        ZLinkCanonicalRelocationAuthorityProjection? canonical = null;
        ZLinkRelocationEnvelope envelope;
        if (ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var currentPublication))
        {
            if (currentPublication.IsCanonical
                && !ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    snapshot.Payload.Span,
                    out canonical))
                throw new ZLinkRelocationDataLostException(
                    $"Actor '{actorId}' canonical relocation authority is malformed.");
            var storedEnvelope = await ZLinkRelocationTreeStore.GetAsync(
                    relocationStore,
                    currentPublication.Reference,
                    currentPublication.ChecksumCrc32c,
                    cancellationToken)
                .ConfigureAwait(false);
            var currentEnvelope = storedEnvelope with
            {
                AggregateGeneration =
                    currentPublication.AggregateGeneration
            };
            if (canonical is not null)
            {
                currentEnvelope = currentEnvelope with
                {
                    CanonicalRelocationHigh = canonical.RelocationHigh,
                    CanonicalRelocationLow = canonical.RelocationLow,
                    Participants = currentEnvelope.Participants.Select(
                            (participant, index) =>
                                participant.CanonicalParticipantId != 0
                                    ? participant
                                    : participant with
                                    {
                                        CanonicalParticipantId =
                                            checked((ulong)index + 1),
                                        AcceptedBoundary = checked(
                                            (ulong)participant.AcceptedJobs.Count),
                                        ReplayCursor = checked(
                                            (ulong)participant.AcceptedJobs.Count)
                                    })
                        .ToArray()
                };
            }
            if (currentEnvelope.AggregateId
                != currentPublication.AggregateId
                || currentEnvelope.Participants.Count != 1
                || ResolveParticipantAuthorityKey(
                    currentEnvelope.Participants[0]) != key)
                throw new ZLinkRelocationDataLostException(
                    $"Actor '{actorId}' Join completion cannot replace another relocation aggregate.");
            var foundParticipant = false;
            envelope = currentEnvelope with
            {
                Participants = currentEnvelope.Participants.Select(
                        participant =>
                        {
                            if (ResolveParticipantAuthorityKey(participant)
                                != key)
                                return participant;
                            foundParticipant = true;
                            return participant with
                            {
                                CompletionPayload =
                                    ZLinkDeferredJoinCompletionCodec.Encode(
                                        completion)
                            };
                        })
                    .ToArray()
            };
            if (!foundParticipant)
                throw new ZLinkRelocationDataLostException(
                    $"Actor '{actorId}' relocation root does not contain its authority participant.");
            replacedReference = currentPublication.Reference;
            replacedRootIsActorOnly =
                currentEnvelope.Participants.Count == 1;
            applicationAuthorityPayload = currentPublication.ApplicationPayload;
        }
        else
        {
            envelope = CreateEnvelope(key, snapshot, completion);
        }
        if (canonical is not null)
            return await PublishCanonicalAsync(
                    key,
                    snapshot,
                    envelope,
                    completion,
                    canonical,
                    replacedReference,
                    replacedRootIsActorOnly,
                    cancellationToken)
                .ConfigureAwait(false);

        var coordinator = new ZLinkRelocationPublicationCoordinator(
            authorityStore,
            relocationStore);
        var published = await coordinator.PublishAsync(
                new ZLinkRelocationPublicationRequest(
                    key,
                    snapshot.StoreVersion,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    snapshot.OwnerId,
                    snapshot.OwnerLeaseGeneration,
                    applicationAuthorityPayload,
                    null,
                    envelope),
                cancellationToken)
            .ConfigureAwait(false);
        if (replacedRootIsActorOnly
            && replacedReference is not null
            && !string.Equals(
                replacedReference,
                published.Relocation.Reference,
                StringComparison.Ordinal))
            await DeleteBestEffortAsync(replacedReference).ConfigureAwait(false);
        return new ZLinkDeferredJoinCompletionRoot(
            key,
            published.Relocation.Reference,
            published.Relocation.ChecksumCrc32c,
            published.Authority.StoreVersion,
            envelope,
            completion);
    }

    internal ValueTask<ZLinkDeferredJoinCompletionRoot> MarkCommittedAsync(
        ZLinkDeferredJoinCompletionRoot root,
        CancellationToken cancellationToken) =>
        MoveCursorAsync(root, ZLinkDeferredJoinCompletionCursor.Committed, cancellationToken);

    internal ValueTask<ZLinkDeferredJoinCompletionRoot> MarkDeliveredAsync(
        ZLinkDeferredJoinCompletionRoot root,
        CancellationToken cancellationToken) =>
        MoveCursorAsync(root, ZLinkDeferredJoinCompletionCursor.Delivered, cancellationToken);

    internal async ValueTask<ZLinkDeferredJoinCompletionRoot?> RecoverAsync(
        string actorId,
        CancellationToken cancellationToken)
    {
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var read = await authorityStore.ReadAuthorityAsync(key, cancellationToken)
            .ConfigureAwait(false);
        return read is ZLinkAuthorityReadResult.Found found
            ? await TryReadPublishedAsync(key, found.Snapshot, cancellationToken)
                .ConfigureAwait(false)
            : null;
    }

    internal async ValueTask<ZLinkAuthoritySnapshot?> ReleaseAsync(
        ZLinkDeferredJoinCompletionRoot root,
        CancellationToken cancellationToken)
    {
        if (root.Completion.Cursor != ZLinkDeferredJoinCompletionCursor.Delivered)
            throw new InvalidOperationException(
                "A deferred Join completion root cannot be released before delivery.");

        var read = await authorityStore.ReadAuthorityAsync(root.AuthorityKey, cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found
            || !ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                found.Snapshot.Payload.Span,
                out var publication)
            || !string.Equals(publication.Reference, root.Reference, StringComparison.Ordinal)
            || publication.ChecksumCrc32c != root.ChecksumCrc32c)
            return null;

        var result = await authorityStore.CompareExchangeAuthorityAsync(
                root.AuthorityKey,
                found.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    publication.ApplicationPayload,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null),
                cancellationToken)
            .ConfigureAwait(false);
        if (result is not ZLinkAuthorityCompareExchangeResult.Stored stored)
            return null;

        await DeleteBestEffortAsync(root.Reference).ConfigureAwait(false);
        return stored.Snapshot;
    }

    private async ValueTask<ZLinkDeferredJoinCompletionRoot> MoveCursorAsync(
        ZLinkDeferredJoinCompletionRoot root,
        ZLinkDeferredJoinCompletionCursor next,
        CancellationToken cancellationToken)
    {
        if ((byte)next < (byte)root.Completion.Cursor
            || (byte)next > (byte)root.Completion.Cursor + 1)
            throw new InvalidOperationException("Deferred Join completion cursor transition is invalid.");
        if (next == root.Completion.Cursor) return root;

        var read = await authorityStore.ReadAuthorityAsync(root.AuthorityKey, cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found)
            throw new ZLinkRelocationDataLostException(
                $"Actor '{root.Completion.ActorId}' authority disappeared during Join completion.");
        var snapshot = found.Snapshot;
        var current = await TryReadPublishedAsync(
                root.AuthorityKey,
                snapshot,
                cancellationToken)
            .ConfigureAwait(false)
            ?? throw new ZLinkRelocationDataLostException(
                $"Actor '{root.Completion.ActorId}' no longer references its Join completion root.");
        EnsureSameOperation(
            current.Completion,
            root.Completion.OperationId,
            root.Completion.Actor);
        if ((byte)current.Completion.Cursor >= (byte)next) return current;

        var updatedCompletion = current.Completion with { Cursor = next };
        var updatedEnvelope = current.Envelope with
        {
            Participants = current.Envelope.Participants.Select(
                    participant => participant.AuthorityKey
                                   == root.AuthorityKey
                        ? participant with
                        {
                    CompletionPayload =
                        ZLinkDeferredJoinCompletionCodec.Encode(
                                    updatedCompletion)
                        }
                        : participant)
                .ToArray()
        };
        var tree = await ZLinkRelocationTreeStore.PutAsync(
                relocationStore,
                updatedEnvelope,
                Retention,
                cancellationToken)
            .ConfigureAwait(false);
        var stored = tree.Root;

        if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var publication))
            throw new ZLinkRelocationDataLostException(
                "Actor authority lost its relocation publication.");
        var nextPayload = publication.IsCanonical
            ? ZLinkCanonicalRelocationAuthorityStateCodec
                .ReplaceRelocationState(
                    snapshot.Payload.Span,
                    ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                        snapshot.Payload.Span,
                        out var canonical)
                        ? canonical.State with
                        {
                            RelocationReference = stored.Reference,
                            RelocationChecksumCrc32c = stored.ChecksumCrc32c
                        }
                        : throw new ZLinkRelocationDataLostException(
                            "Actor authority lost its canonical relocation state."),
                    updatedEnvelope)
            : ZLinkRelocationAuthorityPayloadCodec.Encode(
                publication with
                {
                    Reference = stored.Reference,
                    ChecksumCrc32c = stored.ChecksumCrc32c
                });
        var result = await authorityStore.CompareExchangeAuthorityAsync(
                root.AuthorityKey,
                snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    nextPayload,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null),
                cancellationToken)
            .ConfigureAwait(false);
        if (result is not ZLinkAuthorityCompareExchangeResult.Stored success)
        {
            var recovered = await RecoverAsync(
                    root.Completion.ActorId,
                    cancellationToken)
                .ConfigureAwait(false)
                ?? throw new ZLinkRelocationDataLostException(
                    "Deferred Join completion cursor CAS conflicted without a recoverable root.");
            EnsureSameOperation(
                recovered.Completion,
                root.Completion.OperationId,
                root.Completion.Actor);
            if ((byte)recovered.Completion.Cursor < (byte)next)
                throw new ZLinkRelocationPublicationConflictException(
                    await authorityStore.ReadAuthorityAsync(
                        root.AuthorityKey,
                        cancellationToken)
                    .ConfigureAwait(false));
            if (!string.Equals(
                    recovered.Reference,
                    stored.Reference,
                    StringComparison.Ordinal))
                await DeleteBestEffortAsync(stored.Reference)
                    .ConfigureAwait(false);
            return recovered;
        }

        await DeleteBestEffortAsync(current.Reference).ConfigureAwait(false);
        return new ZLinkDeferredJoinCompletionRoot(
            root.AuthorityKey,
            stored.Reference,
            stored.ChecksumCrc32c,
            success.Snapshot.StoreVersion,
            updatedEnvelope,
            updatedCompletion);
    }

    private async ValueTask<ZLinkDeferredJoinCompletionRoot?> TryReadPublishedAsync(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot snapshot,
        CancellationToken cancellationToken)
    {
        if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var publication))
            return null;
        if (!string.Equals(
                publication.TargetOwnerId,
                snapshot.OwnerId,
                StringComparison.Ordinal)
            || publication.TargetOwnerLeaseGeneration
            != snapshot.OwnerLeaseGeneration)
        {
            if (publication.IsCanonical
                && ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    snapshot.Payload.Span,
                    out var canonical)
                && canonical.Phase is >= 1 and <= 3
                && StringComparer.Ordinal.Equals(
                    canonical.State.SourceOwnerId,
                    snapshot.OwnerId)
                && canonical.State.SourceOwnerLeaseGeneration
                   == checked((ulong)snapshot.OwnerLeaseGeneration))
                return null;
            throw new ZLinkRelocationDataLostException(
                "Deferred Join completion authority owner fence is invalid.");
        }
        var envelope = await ReadRootAsync(
                publication.Reference,
                publication.ChecksumCrc32c,
                cancellationToken)
            .ConfigureAwait(false);
        ZLinkRelocationParticipantEnvelope? participant = null;
        foreach (var candidate in envelope.Participants)
        {
            // A SpotWide relocation root can be referenced by every Actor in
            // the aggregate. Only an Actor participant carrying a completion
            // record can represent deferred Join completion state.
            if (candidate.ObjectKind != ZLinkPlacementObjectKind.Actor
                || candidate.CompletionPayload.IsEmpty)
                continue;
            var actualKey = ResolveParticipantAuthorityKey(candidate);
            if (actualKey != key)
                continue;
            if (participant is not null)
                throw new ZLinkRelocationDataLostException(
                    "Deferred Join completion manifest contains duplicate Actor authority.");
            participant = candidate;
        }
        if (participant is null)
            return null;
        if (participant.ObjectKind != ZLinkPlacementObjectKind.Actor
            || participant.ObjectGeneration != snapshot.ObjectGeneration)
            throw new ZLinkRelocationDataLostException(
                "Deferred Join completion Actor identity is invalid.");
        if (participant.CompletionPayload.IsEmpty)
            return null;
        if (publication.IsCanonical)
        {
            if (!ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    snapshot.Payload.Span,
                    out var canonical))
                throw new ZLinkRelocationDataLostException(
                    "Deferred Join completion canonical authority is invalid.");
            if (canonical.RelocationReference != publication.Reference
                || canonical.RelocationChecksumCrc32c
                   != publication.ChecksumCrc32c)
                throw new ZLinkRelocationDataLostException(
                    "Deferred Join completion canonical reference is invalid.");
            var legacyRootWithoutCanonicalFence =
                envelope.CanonicalRelocationHigh == 0
                && envelope.CanonicalRelocationLow == 0;
            if (envelope.AggregateId != publication.AggregateId
                || !legacyRootWithoutCanonicalFence
                && (envelope.CanonicalRelocationHigh
                    != canonical.RelocationHigh
                    || envelope.CanonicalRelocationLow
                    != canonical.RelocationLow))
                throw new ZLinkRelocationDataLostException(
                    "Deferred Join completion canonical aggregate fence is invalid"
                    + $" (root={envelope.CanonicalRelocationHigh:x16}"
                    + $"{envelope.CanonicalRelocationLow:x16},"
                    + $" authority={canonical.RelocationHigh:x16}"
                    + $"{canonical.RelocationLow:x16}).");
        }
        var completion = ZLinkDeferredJoinCompletionCodec.Decode(
            participant.CompletionPayload.Span);
        return new ZLinkDeferredJoinCompletionRoot(
            key,
            publication.Reference,
            publication.ChecksumCrc32c,
            snapshot.StoreVersion,
            envelope,
            completion);
    }

    private async ValueTask<ZLinkRelocationEnvelope> ReadRootAsync(
        string reference,
        uint checksum,
        CancellationToken cancellationToken)
    {
        var key = (reference, checksum);
        var created = new Lazy<Task<ZLinkRelocationEnvelope>>(
            () => ZLinkRelocationTreeStore.GetAsync(
                    relocationStore,
                    reference,
                    checksum,
                    CancellationToken.None)
                .AsTask(),
            LazyThreadSafetyMode.ExecutionAndPublication);
        var read = _rootReads.GetOrAdd(key, created);
        if (ReferenceEquals(read, created))
            _ = EvictRootReadAsync(key, read);
        try
        {
            return await read.Value.WaitAsync(cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
            _rootReads.TryRemove(
                new KeyValuePair<
                    (string Reference, uint Checksum),
                    Lazy<Task<ZLinkRelocationEnvelope>>>(key, read));
            throw;
        }
    }

    private async Task EvictRootReadAsync(
        (string Reference, uint Checksum) key,
        Lazy<Task<ZLinkRelocationEnvelope>> read)
    {
        try
        {
            await read.Value.ConfigureAwait(false);
            await Task.Delay(TimeSpan.FromSeconds(5)).ConfigureAwait(false);
        }
        catch
        {
            // Failed immutable root reads are immediately eligible for retry.
        }
        _rootReads.TryRemove(
            new KeyValuePair<
                (string Reference, uint Checksum),
                Lazy<Task<ZLinkRelocationEnvelope>>>(key, read));
    }

    internal static ZLinkAuthorityKey ResolveParticipantAuthorityKey(
        ZLinkRelocationParticipantEnvelope participant)
    {
        if (!ZLinkCanonicalParticipantRecoveryCodec.IsEncoded(
                participant.RecoveryPayload.Span))
            return participant.AuthorityKey;
        ZLinkCanonicalParticipantRecovery recovery;
        try
        {
            recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
                participant.RecoveryPayload.Span);
        }
        catch (Exception error) when (error is InvalidDataException
                                      or ArgumentException
                                      or OverflowException)
        {
            throw new ZLinkRelocationDataLostException(
                "Deferred Join completion participant recovery is invalid"
                + $": {error.Message}");
        }
        if (recovery.ObjectKind != participant.ObjectKind
            || recovery.ObjectGeneration != participant.ObjectGeneration)
            throw new ZLinkRelocationDataLostException(
                "Deferred Join completion participant identity is invalid.");
        return recovery.AuthorityKey;
    }

    private async ValueTask<ZLinkDeferredJoinCompletionRoot>
        PublishCanonicalAsync(
            ZLinkAuthorityKey key,
            ZLinkAuthoritySnapshot snapshot,
            ZLinkRelocationEnvelope envelope,
            ZLinkDeferredJoinCompletionRecord completion,
            ZLinkCanonicalRelocationAuthorityProjection canonical,
            string? replacedReference,
            bool replacedRootIsActorOnly,
            CancellationToken cancellationToken)
    {
        var tree = await ZLinkRelocationTreeStore.PutAsync(
                relocationStore,
                envelope,
                Retention,
                cancellationToken)
            .ConfigureAwait(false);
        var stored = tree.Root;
        var nextPayload = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(
                snapshot.Payload.Span,
                canonical.State with
                {
                    RelocationReference = stored.Reference,
                    RelocationChecksumCrc32c = stored.ChecksumCrc32c
                },
                envelope);
        ZLinkAuthorityCompareExchangeResult result;
        try
        {
            result = await authorityStore.CompareExchangeAuthorityAsync(
                    key,
                    snapshot.StoreVersion,
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
            var current = await TryReadAuthorityWithoutCancellationAsync(key)
                .ConfigureAwait(false);
            if (current is not null)
            {
                var recovered = await TryReadPublishedAsync(
                        key,
                        current,
                        CancellationToken.None)
                    .ConfigureAwait(false);
                if (recovered is not null)
                {
                    EnsureSameOperation(
                        recovered.Completion,
                        completion.OperationId,
                        completion.Actor);
                    return recovered;
                }
            }
            throw;
        }

        if (result is ZLinkAuthorityCompareExchangeResult.Stored success)
        {
            if (replacedRootIsActorOnly
                && replacedReference is not null
                && !string.Equals(
                    replacedReference,
                    stored.Reference,
                    StringComparison.Ordinal))
                await DeleteBestEffortAsync(replacedReference)
                    .ConfigureAwait(false);
            return new ZLinkDeferredJoinCompletionRoot(
                key,
                stored.Reference,
                stored.ChecksumCrc32c,
                success.Snapshot.StoreVersion,
                envelope,
                completion);
        }

        if (result is ZLinkAuthorityCompareExchangeResult.Conflict conflict)
        {
            var current = conflict.Current is ZLinkAuthorityReadResult.Found found
                ? found.Snapshot
                : null;
            if (current is not null)
            {
                var recovered = await TryReadPublishedAsync(
                        key,
                        current,
                        cancellationToken)
                    .ConfigureAwait(false);
                if (recovered is not null)
                {
                    EnsureSameOperation(
                        recovered.Completion,
                        completion.OperationId,
                        completion.Actor);
                    if (!string.Equals(
                            recovered.Reference,
                            stored.Reference,
                            StringComparison.Ordinal))
                        await DeleteBestEffortAsync(stored.Reference)
                            .ConfigureAwait(false);
                    return recovered;
                }
            }
            await DeleteBestEffortAsync(stored.Reference).ConfigureAwait(false);
            throw new ZLinkRelocationPublicationConflictException(
                conflict.Current);
        }

        await DeleteBestEffortAsync(stored.Reference).ConfigureAwait(false);
        throw new InvalidOperationException(
            "Authority Store rejected canonical deferred Join completion publication.");
    }

    private async ValueTask<ZLinkAuthoritySnapshot?>
        TryReadAuthorityWithoutCancellationAsync(ZLinkAuthorityKey key)
    {
        try
        {
            var read = await authorityStore.ReadAuthorityAsync(
                    key,
                    CancellationToken.None)
                .ConfigureAwait(false);
            return read is ZLinkAuthorityReadResult.Found found
                ? found.Snapshot
                : null;
        }
        catch
        {
            return null;
        }
    }

    private static ZLinkRelocationEnvelope CreateEnvelope(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot snapshot,
        ZLinkDeferredJoinCompletionRecord completion)
    {
        var inventory = SHA256.HashData(Encoding.UTF8.GetBytes(key.Value));
        return new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            snapshot.AuthorityOwnerGeneration,
            inventory,
            [
                new ZLinkRelocationParticipantEnvelope(
                    key,
                    ZLinkPlacementObjectKind.Actor,
                    snapshot.ObjectGeneration,
                    snapshot.AuthorityOwnerGeneration,
                    ReadOnlyMemory<byte>.Empty,
                    Array.Empty<ZLinkRelocationQueuedJob>(),
                    Array.Empty<ZLinkRelocationLogicalTimer>(),
                    ReadOnlyMemory<byte>.Empty,
                    ZLinkDeferredJoinCompletionCodec.Encode(completion))
            ]);
    }

    private async ValueTask DeleteBestEffortAsync(string reference)
    {
        try
        {
            await ZLinkRelocationTreeStore.DeleteTreeAsync(
                    relocationStore,
                    reference,
                    CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch
        {
            // The fixed retention remains the orphan cleanup boundary.
        }
    }

    private static void ValidateIdentity(
        string actorId,
        ZLinkActorJoinOperationId operationId,
        ActorRef actor)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(actorId);
        if (operationId.High == 0 && operationId.Low == 0)
            throw new ArgumentOutOfRangeException(nameof(operationId));
        if (!string.Equals(actor.ActorId, actorId, StringComparison.Ordinal)
            || actor.ObjectGeneration == 0)
            throw new ArgumentException("Deferred Join completion Actor identity is invalid.");
    }

    private static void EnsureSameOperation(
        ZLinkDeferredJoinCompletionRecord completion,
        ZLinkActorJoinOperationId operationId,
        ActorRef actor)
    {
        if (completion.OperationId != operationId
            || completion.Actor != actor
            || completion.ObjectGeneration != actor.ObjectGeneration)
            throw new InvalidOperationException(
                $"Actor '{actor.ActorId}' already has a different durable Join completion.");
    }
}

internal static class ZLinkDeferredJoinCompletionCodec
{
    private const uint Magic = 0x5a4c4a43; // ZLJC
    private const byte Version = 2;
    private const int MaximumActorIdBytes = 255;
    private const int MaximumReplyBytes = 1024 * 1024;
    private const int MaximumEncodedBytes =
        MaximumReplyBytes
        + sizeof(ushort) + MaximumActorIdBytes
        + 2 * (sizeof(ushort) + ushort.MaxValue)
        + sizeof(byte) + byte.MaxValue
        + 4 * sizeof(ulong)
        + sizeof(uint) + sizeof(int) + 3 * sizeof(byte);

    internal static byte[] Encode(ZLinkDeferredJoinCompletionRecord value)
    {
        if (!string.Equals(
                value.ActorId,
                value.Actor.ActorId,
                StringComparison.Ordinal)
            || Encoding.UTF8.GetByteCount(value.ActorId)
               > MaximumActorIdBytes
            || value.Reply.Length > MaximumReplyBytes)
            throw new InvalidDataException();
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        writer.Write(Magic);
        writer.Write(Version);
        WriteString(writer, value.ActorId);
        writer.Write(value.ObjectGeneration);
        writer.Write(value.OperationId.High);
        writer.Write(value.OperationId.Low);
        WriteString(writer, value.Actor.MeshName);
        WriteRoutingId(writer, value.Actor.NodeRid);
        writer.Write(value.Actor.ObjectGeneration);
        writer.Write(value.ReplyContentType is not null);
        if (value.ReplyContentType is { } contentType) WriteString(writer, contentType);
        writer.Write(value.Reply.Length);
        writer.Write(value.Reply.Span);
        writer.Write((byte)value.Cursor);
        writer.Flush();
        if (stream.Length > MaximumEncodedBytes)
            throw new InvalidDataException();
        return stream.ToArray();
    }

    internal static ZLinkDeferredJoinCompletionRecord Decode(ReadOnlySpan<byte> encoded)
    {
        if (encoded.Length is <= 0 or > MaximumEncodedBytes)
            throw new InvalidDataException();
        using var stream = new MemoryStream(encoded.ToArray(), writable: false);
        using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: true);
        if (reader.ReadUInt32() != Magic || reader.ReadByte() != Version)
            throw new InvalidDataException();
        var actorId = ReadString(reader);
        var generation = reader.ReadUInt64();
        var operation = new ZLinkActorJoinOperationId(
            reader.ReadUInt64(),
            reader.ReadUInt64());
        var meshName = ReadString(reader);
        var nodeRid = ReadRoutingId(reader);
        var actorGeneration = reader.ReadUInt64();
        var contentType = reader.ReadBoolean() ? ReadString(reader) : null;
        var replyLength = reader.ReadInt32();
        if (replyLength < 0 || replyLength > MaximumReplyBytes)
            throw new InvalidDataException();
        var reply = reader.ReadBytes(replyLength);
        if (reply.Length != replyLength) throw new EndOfStreamException();
        var cursor = (ZLinkDeferredJoinCompletionCursor)reader.ReadByte();
        if (generation == 0
            || actorGeneration != generation
            || operation.High == 0 && operation.Low == 0
            || !Enum.IsDefined(cursor)
            || stream.Position != stream.Length)
            throw new InvalidDataException();
        return new ZLinkDeferredJoinCompletionRecord(
            actorId,
            generation,
            operation,
            new ActorRef(actorId, generation, meshName, nodeRid),
            contentType,
            reply,
            cursor);
    }

    private static void WriteString(BinaryWriter writer, string value)
    {
        var bytes = Encoding.UTF8.GetBytes(value);
        if (bytes.Length is < 1 or > ushort.MaxValue) throw new InvalidDataException();
        writer.Write((ushort)bytes.Length);
        writer.Write(bytes);
    }

    private static string ReadString(BinaryReader reader)
    {
        var length = reader.ReadUInt16();
        if (length == 0) throw new InvalidDataException();
        var bytes = reader.ReadBytes(length);
        if (bytes.Length != length) throw new EndOfStreamException();
        return Encoding.UTF8.GetString(bytes);
    }

    private static void WriteRoutingId(BinaryWriter writer, RoutingId value)
    {
        var bytes = value.ToBytes();
        if (bytes.Length is < 1 or > byte.MaxValue) throw new InvalidDataException();
        writer.Write((byte)bytes.Length);
        writer.Write(bytes);
    }

    private static RoutingId ReadRoutingId(BinaryReader reader)
    {
        var length = reader.ReadByte();
        if (length == 0) throw new InvalidDataException();
        var bytes = reader.ReadBytes(length);
        if (bytes.Length != length) throw new EndOfStreamException();
        return RoutingId.From(bytes);
    }
}
