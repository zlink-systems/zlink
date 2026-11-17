namespace Zlink.Framework.Runtime.Locations;

internal sealed record ZLinkRelocationRecoveryCandidate(
    ZLinkRelocationManifestReference Reference,
    ZLinkRelocationEnvelope Envelope,
    IReadOnlyList<ZLinkAuthorityEntry> Authorities
);

/// <summary>
/// Reads the published relocation root from the exact staged participant authorities.
/// </summary>
internal sealed class ZLinkRelocationStartupRecovery(
    IZLinkRuntimeFailureReporter failureReporter,
    IZLinkLocationRepository authorityStore,
    IZLinkRelocationRepository relocationStore
)
{
    internal async ValueTask<ZLinkRelocationRecoveryCandidate?> TryReadExactPublishedAsync(
        ZLinkRelocationEnvelope staging,
        CancellationToken cancellationToken = default
    )
    {
        ArgumentNullException.ThrowIfNull(staging);
        var linked = new Dictionary<string, RecoveryGroup>(StringComparer.Ordinal);
        var unpublished = 0;
        foreach (var participant in staging.Participants)
        {
            var read = await authorityStore
                .ReadAuthorityAsync(participant.AuthorityKey, cancellationToken)
                .ConfigureAwait(false);
            if (read is not ZLinkAuthorityReadResult.Found found)
                throw DataLost(
                    $"Relocation authority '{participant.AuthorityKey.Value}' is unavailable during exact reconciliation."
                );
            var entry = new ZLinkAuthorityEntry(participant.AuthorityKey, found.Snapshot);
            if (!ZLinkRelocationAuthorityPayloadCodec.TryDecode(found.Snapshot.Payload.Span, out _))
            {
                unpublished++;
                continue;
            }
            AddPublished(entry, linked);
        }

        if (unpublished == staging.Participants.Count)
            return null;
        if (unpublished != 0 || linked.Count != 1)
            throw DataLost(
                $"Relocation aggregate '{staging.AggregateId:N}' has a partially visible publication."
            );

        var group = linked.Values.Single();
        ZLinkRelocationEnvelope envelope;
        try
        {
            envelope = await new ZLinkRelocationPublicationCoordinator(
                failureReporter,
                authorityStore,
                relocationStore
            )
                .ReadPreparedAsync(group.Reference, cancellationToken)
                .ConfigureAwait(false);
            envelope = BindManifestGeneration(envelope, group.Reference);
        }
        catch (ZLinkRelocationDataLostException error)
        {
            throw DataLost(error.Message, error);
        }
        ValidateLinkedAuthorities(group.Authorities, envelope);
        return new ZLinkRelocationRecoveryCandidate(
            group.Reference,
            envelope,
            group
                .Authorities.OrderBy(static entry => entry.Key.Value, StringComparer.Ordinal)
                .ToArray()
        );
    }

    private static ZLinkRelocationEnvelope BindManifestGeneration(
        ZLinkRelocationEnvelope envelope,
        ZLinkRelocationManifestReference reference
    ) =>
        envelope.CanonicalLogicalStream.IsEmpty
        || envelope.AggregateGeneration == reference.AggregateGeneration
            ? envelope
            : envelope with
            {
                AggregateGeneration = reference.AggregateGeneration,
            };

    private static void AddPublished(
        ZLinkAuthorityEntry entry,
        Dictionary<string, RecoveryGroup> linked
    )
    {
        if (
            entry.Snapshot.Allocation.ObjectKind
            is not (
                ZLinkPlacementObjectKind.Actor
                or ZLinkPlacementObjectKind.UserSpot
                or ZLinkPlacementObjectKind.InstanceSpot
            )
        )
            return;
        if (
            !ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                entry.Snapshot.Payload.Span,
                out var publication
            )
        )
            return;
        string expectedOwnerId;
        long expectedOwnerLeaseGeneration;
        if (
            ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                entry.Snapshot.Payload.Span,
                out var canonical
            )
        )
        {
            if (canonical.Phase == 1)
                return;
            var sourceOwned = canonical.Phase is 2 or 3 or 9;
            expectedOwnerId = sourceOwned ? canonical.State.SourceOwnerId : canonical.TargetOwnerId;
            expectedOwnerLeaseGeneration = checked(
                (long)(
                    sourceOwned
                        ? canonical.State.SourceOwnerLeaseGeneration
                        : canonical.TargetOwnerLeaseGeneration
                )
            );
        }
        else
        {
            expectedOwnerId = publication.TargetOwnerId;
            expectedOwnerLeaseGeneration = publication.TargetOwnerLeaseGeneration;
        }
        if (
            !string.Equals(expectedOwnerId, entry.Snapshot.OwnerId, StringComparison.Ordinal)
            || expectedOwnerLeaseGeneration != entry.Snapshot.OwnerLeaseGeneration
        )
            throw DataLost(
                $"Authority '{entry.Key.Value}' does not match its published relocation owner fence."
            );

        var reference = new ZLinkRelocationManifestReference(
            publication.Reference,
            publication.ChecksumCrc32c,
            publication.AggregateId,
            publication.AggregateGeneration,
            publication.InventoryDigest
        );
        if (!linked.TryGetValue(reference.Reference, out var group))
        {
            linked.Add(reference.Reference, new RecoveryGroup(reference, [entry]));
            return;
        }
        if (
            group.Reference.ChecksumCrc32c != reference.ChecksumCrc32c
            || group.Reference.AggregateId != reference.AggregateId
            || group.Reference.AggregateGeneration != reference.AggregateGeneration
            || !group.Reference.InventoryDigest.Span.SequenceEqual(reference.InventoryDigest.Span)
        )
            throw DataLost(
                $"Relocation reference '{reference.Reference}' has inconsistent authority manifests."
            );
        group.Authorities.Add(entry);
    }

    private static void ValidateLinkedAuthorities(
        IReadOnlyList<ZLinkAuthorityEntry> authorities,
        ZLinkRelocationEnvelope envelope
    )
    {
        if (!envelope.CanonicalLogicalStream.IsEmpty)
        {
            var standaloneActor =
                envelope.Participants.Count == 1
                && authorities.Count == 1
                && envelope.Participants[0].ObjectKind == ZLinkPlacementObjectKind.Actor
                && authorities[0].Snapshot.Allocation.ObjectKind == ZLinkPlacementObjectKind.Actor;
            if (
                envelope.Participants.Count != authorities.Count
                || !standaloneActor
                    && (
                        authorities.Count(static authority =>
                            authority.Snapshot.Allocation.ObjectKind
                                is ZLinkPlacementObjectKind.UserSpot
                                    or ZLinkPlacementObjectKind.InstanceSpot
                        ) != 1
                        || authorities.Any(static authority =>
                            authority.Snapshot.Allocation.ObjectKind
                                is not (
                                    ZLinkPlacementObjectKind.Actor
                                    or ZLinkPlacementObjectKind.UserSpot
                                    or ZLinkPlacementObjectKind.InstanceSpot
                                )
                        )
                    )
            )
                throw DataLost(
                    $"Canonical relocation aggregate '{envelope.AggregateId:N}' authority inventory is invalid."
                );
            return;
        }
        var participants = envelope.Participants.ToDictionary(
            static participant => participant.AuthorityKey.Value,
            StringComparer.Ordinal
        );
        if (participants.Count != authorities.Count)
            throw DataLost(
                $"Relocation aggregate '{envelope.AggregateId:N}' does not have one published authority per participant."
            );
        foreach (var authority in authorities)
        {
            if (
                !participants.TryGetValue(authority.Key.Value, out var participant)
                || participant.ObjectKind != authority.Snapshot.Allocation.ObjectKind
                || participant.ObjectGeneration != authority.Snapshot.ObjectGeneration
            )
                throw DataLost(
                    $"Relocation root does not contain exact authority participant '{authority.Key.Value}'."
                );
        }
    }

    private static ZLinkFrameworkException DataLost(
        string message,
        Exception? innerException = null
    ) =>
        new(
            ZLinkFrameworkErrorKind.DataLost,
            message,
            retryAdvice: ZLinkRetryAdvice.DoNotRetry,
            innerException
        );

    private sealed record RecoveryGroup(
        ZLinkRelocationManifestReference Reference,
        List<ZLinkAuthorityEntry> Authorities
    );
}
