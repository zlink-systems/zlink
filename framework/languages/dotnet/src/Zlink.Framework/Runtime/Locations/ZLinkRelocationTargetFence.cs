namespace Zlink.Framework.Runtime.Locations;

/// <summary>What a relocation target reads from its expected source fence.</summary>
internal enum ZLinkRelocationTargetFenceReading
{
    /// <summary>The fence is unchanged or the Store gave no answer.</summary>
    Pending = 0,

    /// <summary>The target owner holds the authority record.</summary>
    TargetCommitted = 1,

    /// <summary>
    /// The record moved past the expected source StoreVersion to another
    /// owner (source <c>Preserve</c> or a different target).
    /// </summary>
    FenceChanged = 2,
}

/// <summary>
/// Location runtime §10: the relocation target's authority settled against
/// it — a definitive commit conflict, a source fence that moved to another
/// owner (source <c>Preserve</c>), or the end of the target owner lease. The
/// target discards its staging instead of resubmitting.
/// </summary>
internal sealed class ZLinkRelocationTargetSettledException(
    string message,
    Exception? innerException = null
) : InvalidOperationException(message, innerException);

/// <summary>
/// Location runtime §10: the relocation target decides the fate of its
/// staging from one read of the authority record its NewOwner CAS expects.
/// A Store failure is not an answer; the target keeps its staging and reads
/// again while its owner lease is valid.
/// </summary>
internal static class ZLinkRelocationTargetFence
{
    internal static ValueTask<ZLinkRelocationTargetFenceReading> ReadAsync(
        IZLinkLocationRepository store,
        ZLinkAuthorityKey key,
        string expectedStoreVersion,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken
    ) =>
        ReadAsync(
            store,
            key,
            current => StringComparer.Ordinal.Equals(current.StoreVersion, expectedStoreVersion),
            targetOwner,
            cancellationToken
        );

    /// <param name="holdsSourceFence">
    /// Whether a record not owned by the target still is the exact source
    /// fence the target's NewOwner CAS expects.
    /// </param>
    internal static async ValueTask<ZLinkRelocationTargetFenceReading> ReadAsync(
        IZLinkLocationRepository store,
        ZLinkAuthorityKey key,
        Func<ZLinkAuthoritySnapshot, bool> holdsSourceFence,
        ZLinkLocationOwnerToken targetOwner,
        CancellationToken cancellationToken
    )
    {
        ZLinkAuthorityReadResult read;
        try
        {
            read = await store.ReadAuthorityAsync(key, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception) when (!cancellationToken.IsCancellationRequested)
        {
            return ZLinkRelocationTargetFenceReading.Pending;
        }
        if (read is not ZLinkAuthorityReadResult.Found found)
            throw new ZLinkRelocationDataLostException(
                "The relocation target lost the source authority it expects."
            );
        if (
            StringComparer.Ordinal.Equals(found.Snapshot.OwnerId, targetOwner.OwnerId)
            && found.Snapshot.OwnerLeaseGeneration == targetOwner.LeaseGeneration
        )
            return ZLinkRelocationTargetFenceReading.TargetCommitted;
        return holdsSourceFence(found.Snapshot)
            ? ZLinkRelocationTargetFenceReading.Pending
            : ZLinkRelocationTargetFenceReading.FenceChanged;
    }
}
