using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotRelocationSourceInventory
{
    internal static async ValueTask<ZLinkAuthoritySnapshot> CaptureAsync(
        IZLinkLocationRepository store,
        ZLinkAggregateFence fence,
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot source,
        string digest,
        ulong applicationVersion,
        CancellationToken cancellationToken
    )
    {
        var coordinator = new ZLinkAuthorityRelocation.CoordinatorFence(
            source.Allocation.Descriptor.Rid.ToHex(),
            source.Allocation.DescriptorLifecycleGeneration,
            source.OwnerId,
            checked((ulong)source.OwnerLeaseGeneration),
            source.StoreVersion
        );
        var progress = new ZLinkAuthorityRelocation(
            fence.AggregateId,
            ServiceWireCodec.RelocationPhase.Preparing,
            coordinator.Identity,
            null,
            coordinator,
            digest,
            applicationVersion
        );
        var preparing = await WritePhaseAsync(store, key, source, progress, cancellationToken)
            .ConfigureAwait(false);
        return await WritePhaseAsync(
                store,
                key,
                preparing,
                progress with
                {
                    Phase = ServiceWireCodec.RelocationPhase.Captured,
                },
                cancellationToken
            )
            .ConfigureAwait(false);
    }

    private static async ValueTask<ZLinkAuthoritySnapshot> WritePhaseAsync(
        IZLinkLocationRepository store,
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot source,
        ZLinkAuthorityRelocation relocation,
        CancellationToken cancellationToken
    )
    {
        var result = await store
            .CompareExchangeAuthorityAsync(
                key,
                source.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    source.Payload,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null
                )
                {
                    Relocation = relocation,
                },
                cancellationToken
            )
            .ConfigureAwait(false);
        return result is ZLinkAuthorityCompareExchangeResult.Stored exchanged
            ? exchanged.Snapshot
            : throw new ZLinkRelocationTargetSettledException(
                "A SPOT relocation source authority changed before capture."
            );
    }

    internal static async ValueTask AbortAsync(
        IZLinkLocationRepository store,
        ZLinkAggregateFence fence,
        ZLinkAuthorityKey key,
        CancellationToken cancellationToken
    )
    {
        if (!await ClearSourceAsync(store, fence, key, cancellationToken).ConfigureAwait(false))
            throw new ZLinkRelocationTargetSettledException(
                "SPOT relocation source authority could not be preserved."
            );

        await store.RemoveSealedInventoryAsync(fence, cancellationToken).ConfigureAwait(false);
    }

    private static async ValueTask<bool> ClearSourceAsync(
        IZLinkLocationRepository store,
        ZLinkAggregateFence fence,
        ZLinkAuthorityKey key,
        CancellationToken cancellationToken
    )
    {
        var read = await store.ReadAuthorityAsync(key, cancellationToken).ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found)
            return false;
        var authority = found.Snapshot;
        if (
            authority.Relocation is not { } relocation
            || relocation.RelocationId != fence.AggregateId
        )
            return true;
        if (relocation.Phase == ServiceWireCodec.RelocationPhase.Prepared)
            return await store.AbortAggregateAsync(fence, cancellationToken).ConfigureAwait(false)
                is ZLinkAggregateAbortResult.Aborted
                    or ZLinkAggregateAbortResult.AlreadyAborted;
        if (
            relocation.Phase
            is not (
                ServiceWireCodec.RelocationPhase.Preparing
                or ServiceWireCodec.RelocationPhase.Captured
            )
        )
            return false;
        return await store
                .CompareExchangeAuthorityAsync(
                    key,
                    authority.StoreVersion,
                    new ZLinkAuthorityMutation.Put(
                        authority.Payload,
                        ZLinkAuthorityGenerationTransition.Preserve,
                        null,
                        null
                    )
                    {
                        ClearRelocation = true,
                    },
                    cancellationToken
                )
                .ConfigureAwait(false) is ZLinkAuthorityCompareExchangeResult.Stored;
    }
}
