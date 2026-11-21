using System.Text.Json.Serialization;

namespace Zlink.Framework.Runtime.Locations;

internal sealed partial class ZLinkProviderLocationRepository
{
    private const string EntrySpotIdPrefix = "entry-spot-id";

    private async ValueTask<(
        ZLinkStoreCondition Condition,
        EntrySpotIdClaim? Active,
        DateTimeOffset StoreNow
    )> ObserveEntrySpotClaimAsync(ZLinkStoreKey key, CancellationToken cancellationToken)
    {
        var observation = await _provider.ReadAsync(key, cancellationToken).ConfigureAwait(false);
        if (observation is ZLinkStoreReadResult.Missing missing)
            return (new ZLinkStoreCondition.Missing(key), null, missing.StoreNow);

        var value = ((ZLinkStoreReadResult.Found)observation).Value;
        var claim = Decode<EntrySpotIdClaim>(value.Bytes);
        var lease = await _provider
            .ReadAsync(OwnerKey(claim.OwnerId), cancellationToken)
            .ConfigureAwait(false);
        var owner = new ZLinkLocationOwnerToken(claim.OwnerId, claim.OwnerLeaseGeneration);
        return (
            new ZLinkStoreCondition.Version(key, value.Version),
            IsOwnerLeaseLive(lease, owner) ? claim : null,
            value.StoreNow
        );
    }

    private async ValueTask<bool> AddEntrySpotClaimWriteAsync(
        ZLinkStoreKey descriptorKey,
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkMeshNodeDescriptor? previous,
        List<ZLinkStoreCondition> conditions,
        List<ZLinkStoreMutation> mutations,
        CancellationToken cancellationToken
    )
    {
        if (descriptor.EntrySpotId is { } spotId)
        {
            var claimKey = OpaqueRecordKey(EntrySpotIdPrefix, spotId);
            var existing = await ObserveEntrySpotClaimAsync(claimKey, cancellationToken)
                .ConfigureAwait(false);
            conditions.Add(existing.Condition);
            if (existing.Active is { } active)
            {
                if (!active.Matches(descriptorKey, descriptor))
                    return false;
            }
            else
            {
                var authorityKey = AuthorityMetaKey(ZLinkAuthorityKeyCodec.EncodeSpot(spotId));
                if (
                    await _provider.ReadAsync(authorityKey, cancellationToken).ConfigureAwait(false)
                    is ZLinkStoreReadResult.Found
                )
                    return false;

                conditions.Add(new ZLinkStoreCondition.Missing(authorityKey));
                mutations.Add(
                    new ZLinkStoreMutation.Put(
                        claimKey,
                        Encode(
                            new EntrySpotIdClaim(
                                descriptorKey.Value,
                                descriptor.LifecycleGeneration,
                                descriptor.OwnerId,
                                descriptor.LeaseGeneration
                            )
                        ),
                        null
                    )
                );
            }
        }

        if (previous is not null && previous.EntrySpotId != descriptor.EntrySpotId)
            await AddEntrySpotClaimRemoveAsync(
                    descriptorKey,
                    previous,
                    conditions,
                    mutations,
                    cancellationToken
                )
                .ConfigureAwait(false);
        return true;
    }

    private async ValueTask AddEntrySpotClaimRemoveAsync(
        ZLinkStoreKey descriptorKey,
        ZLinkMeshNodeDescriptor descriptor,
        List<ZLinkStoreCondition> conditions,
        List<ZLinkStoreMutation> mutations,
        CancellationToken cancellationToken
    )
    {
        if (descriptor.EntrySpotId is not { } spotId)
            return;

        var claimKey = OpaqueRecordKey(EntrySpotIdPrefix, spotId);
        var read = await _provider.ReadAsync(claimKey, cancellationToken).ConfigureAwait(false);
        if (read is ZLinkStoreReadResult.Missing)
        {
            conditions.Add(new ZLinkStoreCondition.Missing(claimKey));
            return;
        }

        var value = ((ZLinkStoreReadResult.Found)read).Value;
        conditions.Add(new ZLinkStoreCondition.Version(claimKey, value.Version));
        if (Decode<EntrySpotIdClaim>(value.Bytes).Matches(descriptorKey, descriptor))
            mutations.Add(new ZLinkStoreMutation.Delete(claimKey));
    }

    private async ValueTask ReleaseOwnerEntrySpotClaimsAsync(
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken
    )
    {
        var leaseKey = OwnerKey(owner.OwnerId);
        var lease = await _provider.ReadAsync(leaseKey, cancellationToken).ConfigureAwait(false);
        if (!IsOwnerLeaseLive(lease, owner))
            return;

        var rows = await ReadOwnerCleanupRowsAsync(
                OpaqueRecordPrefix(EntrySpotIdPrefix),
                cancellationToken
            )
            .ConfigureAwait(false);
        var ownerCondition = new ZLinkStoreCondition.Value(
            leaseKey,
            Encode(new OwnerRecord(owner.OwnerId, owner.LeaseGeneration))
        );
        foreach (var (key, value) in rows)
        {
            var claim = Decode<EntrySpotIdClaim>(value.Bytes);
            if (
                claim.OwnerId != owner.OwnerId
                || claim.OwnerLeaseGeneration != owner.LeaseGeneration
            )
                continue;

            var descriptorKey = Key(claim.DescriptorKey);
            if (
                await _provider.ReadAsync(descriptorKey, cancellationToken).ConfigureAwait(false)
                is not ZLinkStoreReadResult.Found descriptorRow
            )
                continue;
            var descriptor = DecodeDescriptor<ZLinkMeshNodeDescriptor>(
                descriptorRow.Value.Bytes
            ).Descriptor;
            if (
                descriptor.EntrySpotId is not { } spotId
                || key != OpaqueRecordKey(EntrySpotIdPrefix, spotId)
                || !claim.Matches(descriptorKey, descriptor)
            )
                continue;

            await _provider
                .WriteAsync(
                    new ZLinkStoreWriteRequest(
                        [
                            ownerCondition,
                            new ZLinkStoreCondition.Version(
                                descriptorKey,
                                descriptorRow.Value.Version
                            ),
                            new ZLinkStoreCondition.Version(key, value.Version),
                        ],
                        [new ZLinkStoreMutation.Delete(key)]
                    ),
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
    }

    private sealed record EntrySpotIdClaim(
        [property: JsonPropertyName("descriptorKey")] string DescriptorKey,
        [property: JsonPropertyName("descriptorLifecycleGeneration")]
        [property: JsonConverter(
            typeof(ZLinkJsonSerializerOptions.FrameworkUnsigned64JsonConverter)
        )]
            ulong DescriptorLifecycleGeneration,
        [property: JsonPropertyName("ownerId")] string OwnerId,
        [property: JsonPropertyName("ownerLeaseGeneration")]
        [property: JsonConverter(typeof(ZLinkJsonSerializerOptions.FrameworkSigned64JsonConverter))]
            long OwnerLeaseGeneration
    )
    {
        internal bool Matches(ZLinkStoreKey descriptorKey, ZLinkMeshNodeDescriptor descriptor) =>
            DescriptorKey == descriptorKey.Value
            && DescriptorLifecycleGeneration == descriptor.LifecycleGeneration
            && OwnerId == descriptor.OwnerId
            && OwnerLeaseGeneration == descriptor.LeaseGeneration;
    }
}
