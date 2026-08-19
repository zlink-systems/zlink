using System.Globalization;
using System.Runtime.ExceptionServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Translates framework-owned location records to the provider's opaque
/// conditional key/value operations.
/// </summary>
internal sealed partial class ZLinkProviderLocationRepository(
    IZLinkLocationStore provider) : IZLinkLocationRepository
{
    private const string Prefix = "zlink:v11:";
    private const long MaximumGeneration = long.MaxValue;
    private static readonly ZLinkStoreKey OwnerCounterKey =
        Key($"{Prefix}owner-counter");

    public async ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
        string ownerId,
        TimeSpan leaseTtl,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(ownerId);
        if (leaseTtl <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(leaseTtl));

        var ownerKey = OwnerKey(ownerId);
        while (true)
        {
            var owner = await provider.ReadAsync(ownerKey, cancellationToken)
                .ConfigureAwait(false);
            if (owner is ZLinkStoreReadResult.Found)
                return new ZLinkOwnerLeaseClaimResult.Conflict();

            var counter = await provider.ReadAsync(
                    OwnerCounterKey,
                    cancellationToken)
                .ConfigureAwait(false);
            var generation = counter switch
            {
                ZLinkStoreReadResult.Missing => 1L,
                ZLinkStoreReadResult.Found found =>
                    DecodeOwnerGenerationCounter(found.Value.Bytes),
                _ => throw new InvalidOperationException()
            };
            if (generation == MaximumGeneration)
                return new ZLinkOwnerLeaseClaimResult.GenerationExhausted();

            var token = new ZLinkLocationOwnerToken(ownerId, generation);
            ZLinkStoreWriteResult result;
            try
            {
                result = await provider.WriteAsync(
                        new ZLinkStoreWriteRequest(
                        [
                            new ZLinkStoreCondition.Missing(ownerKey),
                            counter switch
                            {
                                ZLinkStoreReadResult.Missing =>
                                    new ZLinkStoreCondition.Missing(OwnerCounterKey),
                                ZLinkStoreReadResult.Found found =>
                                    new ZLinkStoreCondition.Version(
                                        OwnerCounterKey,
                                        found.Value.Version),
                                _ => throw new InvalidOperationException()
                            }
                        ],
                        [
                            new ZLinkStoreMutation.Put(
                                ownerKey,
                                JsonSerializer.SerializeToUtf8Bytes(
                                    new OwnerRecord(
                                        ownerId,
                                        token.LeaseGeneration)),
                                leaseTtl),
                            new ZLinkStoreMutation.Put(
                                OwnerCounterKey,
                                Encoding.UTF8.GetBytes(
                                    checked(generation + 1).ToString(
                                        CultureInfo.InvariantCulture)),
                                null)
                        ]),
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (Exception failure) when (
                failure is not OutOfMemoryException
                and not StackOverflowException
                and not AccessViolationException)
            {
                try
                {
                    using var deadline = new CancellationTokenSource(
                        AmbiguousReconciliationTimeout);
                    var read = await provider.ReadAsync(ownerKey, deadline.Token)
                        .AsTask()
                        .WaitAsync(deadline.Token)
                        .ConfigureAwait(false);
                    if (read is ZLinkStoreReadResult.Found found)
                    {
                        var record = DecodeOwner(found.Value.Bytes);
                        if (string.Equals(
                                record.OwnerId,
                                ownerId,
                                StringComparison.Ordinal)
                            && record.LeaseGeneration == token.LeaseGeneration
                            && found.Value.ExpiresAt is { } expiresAt)
                        {
                            return new ZLinkOwnerLeaseClaimResult.Claimed(
                                token,
                                expiresAt,
                                found.Value.StoreNow);
                        }
                    }
                }
                catch
                {
                    ExceptionDispatchInfo.Capture(failure).Throw();
                }

                ExceptionDispatchInfo.Capture(failure).Throw();
                throw;
            }
            if (result is ZLinkStoreWriteResult.Conflict) continue;
            var applied = (ZLinkStoreWriteResult.Applied)result;
            return new ZLinkOwnerLeaseClaimResult.Claimed(
                token,
                applied.StoreNow + leaseTtl,
                applied.StoreNow);
        }
    }

    private static long DecodeOwnerGenerationCounter(ReadOnlyMemory<byte> bytes)
    {
        var text = Encoding.UTF8.GetString(bytes.Span);
        if (!long.TryParse(
                text,
                NumberStyles.None,
                CultureInfo.InvariantCulture,
                out var value)
            || value is <= 0 or > MaximumGeneration
            || !string.Equals(
                text,
                value.ToString(CultureInfo.InvariantCulture),
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The Location Store owner generation counter is not canonical decimal.");
        }

        return value;
    }

    public async ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
        string ownerId,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(ownerId);
        var result = await provider.ReadAsync(
                OwnerKey(ownerId),
                cancellationToken)
            .ConfigureAwait(false);
        if (result is ZLinkStoreReadResult.Missing)
            return new ZLinkOwnerLeaseReadResult.Missing();
        var found = ((ZLinkStoreReadResult.Found)result).Value;
        var record = DecodeOwner(found.Bytes);
        if (!string.Equals(record.OwnerId, ownerId, StringComparison.Ordinal)
            || found.ExpiresAt is null)
        {
            throw new InvalidDataException(
                "The Location Store owner lease record is invalid.");
        }
        return new ZLinkOwnerLeaseReadResult.Found(
            new ZLinkLocationOwnerToken(
                record.OwnerId,
                record.LeaseGeneration),
            found.ExpiresAt.Value,
            found.StoreNow);
    }

    public async ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
        ZLinkLocationOwnerToken token,
        TimeSpan leaseTtl,
        CancellationToken cancellationToken = default)
    {
        if (leaseTtl <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(leaseTtl));
        var key = OwnerKey(token.OwnerId);
        var current = await provider.ReadAsync(key, cancellationToken)
            .ConfigureAwait(false);
        if (current is not ZLinkStoreReadResult.Found found)
            return new ZLinkOwnerLeaseRenewResult.Stale();
        if (DecodeOwner(found.Value.Bytes).LeaseGeneration
            != token.LeaseGeneration)
            return new ZLinkOwnerLeaseRenewResult.Stale();

        var result = await provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                [
                    new ZLinkStoreCondition.Version(
                        key,
                        found.Value.Version)
                ],
                [
                    new ZLinkStoreMutation.Put(
                        key,
                        found.Value.Bytes,
                        leaseTtl)
                ]),
                cancellationToken)
            .ConfigureAwait(false);
        if (result is ZLinkStoreWriteResult.Conflict)
            return new ZLinkOwnerLeaseRenewResult.Stale();
        var applied = (ZLinkStoreWriteResult.Applied)result;
        return new ZLinkOwnerLeaseRenewResult.Renewed(
            applied.StoreNow + leaseTtl,
            applied.StoreNow);
    }

    public async ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
        ZLinkLocationOwnerToken token,
        CancellationToken cancellationToken = default)
    {
        var key = OwnerKey(token.OwnerId);
        var current = await provider.ReadAsync(key, cancellationToken)
            .ConfigureAwait(false);
        if (current is not ZLinkStoreReadResult.Found found)
            return ZLinkOwnerLeaseReleaseResult.Stale;
        if (DecodeOwner(found.Value.Bytes).LeaseGeneration
            != token.LeaseGeneration)
            return ZLinkOwnerLeaseReleaseResult.Stale;
        var result = await provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                [
                    new ZLinkStoreCondition.Version(
                        key,
                        found.Value.Version)
                ],
                [new ZLinkStoreMutation.Delete(key)]),
                cancellationToken)
            .ConfigureAwait(false);
        return result is ZLinkStoreWriteResult.Applied
            ? ZLinkOwnerLeaseReleaseResult.Released
            : ZLinkOwnerLeaseReleaseResult.Stale;
    }

    public async ValueTask<ZLinkLocationWriteResult> UpdateMeshNodeAsync(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationWriteIntent intent,
        CancellationToken cancellationToken = default)
    {
        var leaseKey = OwnerKey(descriptor.OwnerId);
        var rowKey = MeshKey(descriptor.MeshName, descriptor.Rid);
        var lease = await provider.ReadAsync(leaseKey, cancellationToken)
            .ConfigureAwait(false);
        if (lease is not ZLinkStoreReadResult.Found liveLease)
            return ZLinkLocationWriteResult.IgnoredStale;
        if (DecodeOwner(liveLease.Value.Bytes).LeaseGeneration
            != descriptor.LeaseGeneration)
            return ZLinkLocationWriteResult.IgnoredStale;

        var current = await provider.ReadAsync(rowKey, cancellationToken)
            .ConfigureAwait(false);
        ZLinkStoreCondition rowCondition;
        if (current is ZLinkStoreReadResult.Found found)
        {
            var record = DecodeDescriptor<ZLinkMeshNodeDescriptor>(found.Value.Bytes);
            var stored = record.Descriptor;
            var renew = intent == ZLinkLocationWriteIntent.Renew
                        && stored.OwnerId == descriptor.OwnerId
                        && stored.LeaseGeneration == descriptor.LeaseGeneration
                        && stored.LifecycleGeneration
                        == descriptor.LifecycleGeneration
                        && descriptor.DescriptorRevision
                        > stored.DescriptorRevision;
            if (!renew)
            {
                if (intent == ZLinkLocationWriteIntent.NewClaim)
                    return ZLinkLocationWriteResult.RejectedConflict;
                var previousOwner = await provider.ReadAsync(
                        OwnerKey(stored.OwnerId),
                        cancellationToken)
                    .ConfigureAwait(false);
                if (previousOwner is ZLinkStoreReadResult.Found)
                    return ZLinkLocationWriteResult.IgnoredStale;
            }
            rowCondition = new ZLinkStoreCondition.Version(
                rowKey,
                found.Value.Version);
        }
        else
        {
            if (intent == ZLinkLocationWriteIntent.Renew)
                return ZLinkLocationWriteResult.IgnoredStale;
            rowCondition = new ZLinkStoreCondition.Missing(rowKey);
        }

        var encodedDescriptor = JsonSerializer.SerializeToUtf8Bytes(
            new DescriptorRecord<ZLinkMeshNodeDescriptor>(
                descriptor.OwnerId,
                descriptor.LeaseGeneration,
                descriptor.DescriptorRevision,
                descriptor),
            ZLinkJsonSerializerOptions.Default);
        return await WriteDescriptorWithReconciliationAsync(
                rowKey,
                new ZLinkStoreWriteRequest(
                [
                    new ZLinkStoreCondition.Version(
                        leaseKey,
                        liveLease.Value.Version),
                    rowCondition
                ],
                [
                    new ZLinkStoreMutation.Put(
                        rowKey,
                        encodedDescriptor,
                        null)
                ]),
                encodedDescriptor,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask<ZLinkLocationWriteStatus> RemoveMeshNodeAsync(
        ZLinkMeshNodeDescriptorKey key,
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default)
    {
        var rowKey = MeshKey(key.MeshName, key.Rid);
        var current = await provider.ReadAsync(rowKey, cancellationToken)
            .ConfigureAwait(false);
        if (current is not ZLinkStoreReadResult.Found found)
            return ZLinkLocationWriteStatus.IgnoredStale;
        var descriptor =
            DecodeDescriptor<ZLinkMeshNodeDescriptor>(found.Value.Bytes)
                .Descriptor;
        if (descriptor.OwnerId != owner.OwnerId
            || descriptor.LeaseGeneration != owner.LeaseGeneration)
        {
            return ZLinkLocationWriteStatus.IgnoredStale;
        }
        var result = await provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                [
                    new ZLinkStoreCondition.Version(
                        rowKey,
                        found.Value.Version)
                ],
                [new ZLinkStoreMutation.Delete(rowKey)]),
                cancellationToken)
            .ConfigureAwait(false);
        return result is ZLinkStoreWriteResult.Applied
            ? ZLinkLocationWriteStatus.Stored
            : ZLinkLocationWriteStatus.IgnoredStale;
    }

    public async ValueTask<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>
        ListMeshNodesAsync(
            string meshName,
            ZLinkPageRequest page,
            CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(meshName);
        return await ListCompleteSnapshotPageAsync(
                MeshPrefix(meshName),
                page,
                static bytes =>
                    DecodeDescriptor<ZLinkMeshNodeDescriptor>(bytes).Descriptor,
                cancellationToken)
            .ConfigureAwait(false);
    }

    public ValueTask<ZLinkLocationWriteResult> UpdateClientServerAsync(
        ZLinkClientServerServerDescriptor descriptor,
        ZLinkLocationWriteIntent intent,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(descriptor.ChannelName);
        ArgumentException.ThrowIfNullOrWhiteSpace(descriptor.Endpoint);
        ArgumentException.ThrowIfNullOrWhiteSpace(descriptor.OwnerId);
        if (descriptor.ServerRid.IsEmpty
            || descriptor.LifecycleGeneration == 0
            || descriptor.DescriptorRevision == 0
            || descriptor.Weight is < 0 or > ZLinkSocketConfig.MaximumPeerWeight)
            throw new ArgumentOutOfRangeException(nameof(descriptor));
        return UpdateDescriptorAsync(
            ClientServerKey(descriptor.ChannelName, descriptor.ServerRid),
            descriptor.OwnerId,
            descriptor.LeaseGeneration,
            descriptor.LifecycleGeneration,
            descriptor.DescriptorRevision,
            descriptor,
            intent,
            static d => d.LifecycleGeneration,
            static (current, next) =>
                current.Endpoint == next.Endpoint
                && current.SecurityIdentity == next.SecurityIdentity,
            cancellationToken);
    }

    public ValueTask<ZLinkLocationWriteStatus> RemoveClientServerAsync(
        ZLinkClientServerServerDescriptorKey key,
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default) =>
        RemoveDescriptorAsync<ZLinkClientServerServerDescriptor>(
            ClientServerKey(key.ChannelName, key.ServerRid),
            owner,
            static descriptor => descriptor.OwnerId,
            static descriptor => descriptor.LeaseGeneration,
            cancellationToken);

    public ValueTask<ZLinkLocationPage<ZLinkClientServerServerDescriptor>>
        ListClientServersAsync(
            string channelName,
            ZLinkPageRequest page,
            CancellationToken cancellationToken = default) =>
        ListDescriptorsAsync<ZLinkClientServerServerDescriptor>(
            ClientServerPrefix(channelName),
            page,
            cancellationToken);

    public ValueTask<ZLinkLocationWriteResult> UpdateFanoutPublisherAsync(
        ZLinkFanoutPublisherDescriptor descriptor,
        ZLinkLocationWriteIntent intent,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(descriptor.ChannelName);
        ArgumentException.ThrowIfNullOrWhiteSpace(descriptor.Endpoint);
        ArgumentException.ThrowIfNullOrWhiteSpace(descriptor.OwnerId);
        if (descriptor.PublisherRid.IsEmpty
            || descriptor.LifecycleGeneration == 0
            || descriptor.DescriptorRevision == 0)
            throw new ArgumentOutOfRangeException(nameof(descriptor));
        return UpdateDescriptorAsync(
            FanoutKey(descriptor.ChannelName, descriptor.PublisherRid),
            descriptor.OwnerId,
            descriptor.LeaseGeneration,
            descriptor.LifecycleGeneration,
            descriptor.DescriptorRevision,
            descriptor,
            intent,
            static d => d.LifecycleGeneration,
            static (current, next) =>
                current.Endpoint == next.Endpoint
                && current.SecurityIdentity == next.SecurityIdentity,
            cancellationToken);
    }

    public ValueTask<ZLinkLocationWriteStatus> RemoveFanoutPublisherAsync(
        ZLinkFanoutPublisherDescriptorKey key,
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default) =>
        RemoveDescriptorAsync<ZLinkFanoutPublisherDescriptor>(
            FanoutKey(key.ChannelName, key.PublisherRid),
            owner,
            static descriptor => descriptor.OwnerId,
            static descriptor => descriptor.LeaseGeneration,
            cancellationToken);

    public ValueTask<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>>
        ListFanoutPublishersAsync(
            string channelName,
            ZLinkPageRequest page,
            CancellationToken cancellationToken = default) =>
        ListDescriptorsAsync<ZLinkFanoutPublisherDescriptor>(
            FanoutPrefix(channelName),
            page,
            cancellationToken);

    public ValueTask<ulong?> GetMeshNodeChangeStampAsync(
        string meshName,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(meshName);
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult<ulong?>(null);
    }

    public async ValueTask<long> RemoveAllByOwnerAsync(
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(owner.OwnerId);
        if (owner.LeaseGeneration <= 0)
            throw new ArgumentOutOfRangeException(nameof(owner));

        var removed = 0L;
        removed += await RemoveOwnedDescriptorsAsync(
                "mesh-node\0",
                owner,
                static bytes =>
                {
                    var descriptor =
                        DecodeDescriptor<ZLinkMeshNodeDescriptor>(bytes)
                            .Descriptor;
                    return new ZLinkLocationOwnerToken(
                        descriptor.OwnerId,
                        descriptor.LeaseGeneration);
                },
                cancellationToken)
            .ConfigureAwait(false);
        removed += await RemoveOwnedDescriptorsAsync(
                "client-server\0",
                owner,
                static bytes =>
                {
                    var descriptor = DecodeDescriptor<
                        ZLinkClientServerServerDescriptor>(
                            bytes).Descriptor;
                    return new ZLinkLocationOwnerToken(
                        descriptor.OwnerId,
                        descriptor.LeaseGeneration);
                },
                cancellationToken)
            .ConfigureAwait(false);
        removed += await RemoveOwnedDescriptorsAsync(
                "fanout-publisher\0",
                owner,
                static bytes =>
                {
                    var descriptor = DecodeDescriptor<
                        ZLinkFanoutPublisherDescriptor>(
                            bytes).Descriptor;
                    return new ZLinkLocationOwnerToken(
                        descriptor.OwnerId,
                        descriptor.LeaseGeneration);
                },
                cancellationToken)
            .ConfigureAwait(false);
        return removed;
    }

    private async ValueTask<long> RemoveOwnedDescriptorsAsync(
        string prefix,
        ZLinkLocationOwnerToken owner,
        Func<ReadOnlyMemory<byte>, ZLinkLocationOwnerToken> readOwner,
        CancellationToken cancellationToken)
    {
        while (true)
        {
            var rows = new List<KeyValuePair<ZLinkStoreKey, ZLinkStoreValue>>();
            ZLinkStoreScanCursor? cursor = null;
            var expired = false;
            do
            {
                var result = await provider.ScanAsync(
                        new ZLinkStoreScanRequest(prefix, cursor, 1000),
                        cancellationToken)
                    .ConfigureAwait(false);
                if (result is ZLinkStoreScanResult.Expired)
                {
                    expired = true;
                    break;
                }
                var page = ((ZLinkStoreScanResult.Page)result).Value;
                rows.AddRange(page.Items);
                cursor = page.NextCursor;
            } while (cursor is not null);
            if (expired)
                continue;

            var removed = 0L;
            foreach (var row in rows)
            {
                if (readOwner(row.Value.Bytes) != owner)
                    continue;
                var result = await provider.WriteAsync(
                        new ZLinkStoreWriteRequest(
                        [
                            new ZLinkStoreCondition.Version(
                                row.Key,
                                row.Value.Version)
                        ],
                        [new ZLinkStoreMutation.Delete(row.Key)]),
                        cancellationToken)
                    .ConfigureAwait(false);
                if (result is ZLinkStoreWriteResult.Applied)
                    removed++;
            }
            return removed;
        }
    }

    private async ValueTask<ZLinkLocationWriteResult> UpdateDescriptorAsync<T>(
        ZLinkStoreKey rowKey,
        string ownerId,
        long leaseGeneration,
        ulong lifecycleGeneration,
        ulong descriptorRevision,
        T descriptor,
        ZLinkLocationWriteIntent intent,
        Func<T, ulong> lifecycleGenerationOf,
        Func<T, T, bool> immutableFieldsEqual,
        CancellationToken cancellationToken)
    {
        var leaseKey = OwnerKey(ownerId);
        var lease = await provider.ReadAsync(leaseKey, cancellationToken)
            .ConfigureAwait(false);
        if (lease is not ZLinkStoreReadResult.Found liveLease
            || DecodeOwner(liveLease.Value.Bytes).LeaseGeneration
            != leaseGeneration)
            return ZLinkLocationWriteResult.IgnoredStale;

        var current = await provider.ReadAsync(rowKey, cancellationToken)
            .ConfigureAwait(false);
        ZLinkStoreCondition rowCondition;
        if (current is ZLinkStoreReadResult.Found found)
        {
            var record = DecodeDescriptor<T>(found.Value.Bytes);
            var storedOwner = record.OwnerId;
            var storedLeaseGeneration = record.LeaseGeneration;
            var storedOwnerLease = await provider.ReadAsync(
                    OwnerKey(storedOwner),
                    cancellationToken)
                .ConfigureAwait(false);
            var storedOwnerLive = storedOwnerLease is ZLinkStoreReadResult.Found;
            if (intent == ZLinkLocationWriteIntent.NewClaim && storedOwnerLive)
                return ZLinkLocationWriteResult.RejectedConflict;
            if (intent == ZLinkLocationWriteIntent.Takeover && storedOwnerLive)
                return ZLinkLocationWriteResult.IgnoredStale;
            if (intent == ZLinkLocationWriteIntent.Renew
                && (storedOwner != ownerId
                    || storedLeaseGeneration != leaseGeneration
                    || lifecycleGenerationOf(record.Descriptor)
                    != lifecycleGeneration
                    || descriptorRevision <= record.DescriptorRevision
                    || !immutableFieldsEqual(record.Descriptor, descriptor)))
                return ZLinkLocationWriteResult.IgnoredStale;
            rowCondition = new ZLinkStoreCondition.Version(
                rowKey,
                found.Value.Version);
        }
        else
        {
            if (intent == ZLinkLocationWriteIntent.Renew)
                return ZLinkLocationWriteResult.IgnoredStale;
            rowCondition = new ZLinkStoreCondition.Missing(rowKey);
        }

        var encodedDescriptor = JsonSerializer.SerializeToUtf8Bytes(
            new DescriptorRecord<T>(
                ownerId,
                leaseGeneration,
                descriptorRevision,
                descriptor),
            ZLinkJsonSerializerOptions.Default);
        return await WriteDescriptorWithReconciliationAsync(
                rowKey,
                new ZLinkStoreWriteRequest(
                [
                    new ZLinkStoreCondition.Version(
                        leaseKey,
                        liveLease.Value.Version),
                    rowCondition
                ],
                [
                    new ZLinkStoreMutation.Put(
                        rowKey,
                        encodedDescriptor,
                        null)
                ]),
                encodedDescriptor,
                cancellationToken)
            .ConfigureAwait(false);
    }

    // The framework no longer keeps its own generation counter inside the
    // descriptor payload (21-location-runtime.md#2.4: "a provider-internal
    // storage-row version counter... isn't added to this canonical JSON,
    // because the opaque record's own... version member already fills that
    // role") -- the Generation this repository hands back to a caller is
    // derived from the provider's own per-key version returned by the write
    // (or, on an idempotent-replay reconciliation, the post-read version of
    // the row actually on the store), the same "version_of" pattern the cpp
    // store closure landed for storeVersion. Every caller of Update*Async's
    // result only ever treats Generation as a "did this write actually get
    // applied" nonzero marker (ZLinkAutoConnectReconciler's _localGeneration
    // gate), never as a real sequence number, so VersionOf below falls back
    // to a stable 1 when the provider's version string isn't a plain
    // decimal integer -- true for the live Redis provider, whose per-key Put
    // version is a freshly issued GUID, not a counter.
    private static ulong VersionOf(ZLinkStoreVersion version) =>
        ulong.TryParse(
            version.Value,
            NumberStyles.None,
            CultureInfo.InvariantCulture,
            out var parsed) && parsed != 0
            ? parsed
            : 1;

    private async ValueTask<ZLinkLocationWriteResult>
        WriteDescriptorWithReconciliationAsync(
            ZLinkStoreKey rowKey,
            ZLinkStoreWriteRequest request,
            ReadOnlyMemory<byte> encodedDescriptor,
            CancellationToken cancellationToken)
    {
        ZLinkStoreWriteResult result;
        try
        {
            result = await provider.WriteAsync(request, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (Exception failure) when (
            failure is not OutOfMemoryException
            and not StackOverflowException
            and not AccessViolationException)
        {
            try
            {
                using var deadline = new CancellationTokenSource(
                    AmbiguousReconciliationTimeout);
                var read = await provider.ReadAsync(rowKey, deadline.Token)
                    .AsTask()
                    .WaitAsync(deadline.Token)
                    .ConfigureAwait(false);
                if (read is ZLinkStoreReadResult.Found found
                    && found.Value.Bytes.Span.SequenceEqual(
                        encodedDescriptor.Span))
                {
                    return ZLinkLocationWriteResult.Stored(
                        VersionOf(found.Value.Version),
                        found.Value.StoreNow);
                }
            }
            catch
            {
                ExceptionDispatchInfo.Capture(failure).Throw();
            }

            ExceptionDispatchInfo.Capture(failure).Throw();
            throw;
        }

        return result is ZLinkStoreWriteResult.Applied applied
            ? ZLinkLocationWriteResult.Stored(
                VersionOf(applied.PutVersions[rowKey]),
                applied.StoreNow)
            : ZLinkLocationWriteResult.IgnoredStale;
    }

    private async ValueTask<ZLinkLocationWriteStatus> RemoveDescriptorAsync<T>(
        ZLinkStoreKey rowKey,
        ZLinkLocationOwnerToken owner,
        Func<T, string> ownerId,
        Func<T, long> leaseGeneration,
        CancellationToken cancellationToken)
    {
        var current = await provider.ReadAsync(rowKey, cancellationToken)
            .ConfigureAwait(false);
        if (current is not ZLinkStoreReadResult.Found found)
            return ZLinkLocationWriteStatus.IgnoredStale;
        var record = DecodeDescriptor<T>(found.Value.Bytes);
        if (ownerId(record.Descriptor) != owner.OwnerId
            || leaseGeneration(record.Descriptor) != owner.LeaseGeneration)
            return ZLinkLocationWriteStatus.IgnoredStale;
        var result = await provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                [
                    new ZLinkStoreCondition.Version(
                        rowKey,
                        found.Value.Version)
                ],
                [new ZLinkStoreMutation.Delete(rowKey)]),
                cancellationToken)
            .ConfigureAwait(false);
        return result is ZLinkStoreWriteResult.Applied
            ? ZLinkLocationWriteStatus.Stored
            : ZLinkLocationWriteStatus.IgnoredStale;
    }

    private async ValueTask<ZLinkLocationPage<T>> ListDescriptorsAsync<T>(
        string prefix,
        ZLinkPageRequest page,
        CancellationToken cancellationToken)
    {
        return await ListCompleteSnapshotPageAsync(
                prefix,
                page,
                static bytes => DecodeDescriptor<T>(bytes).Descriptor,
                cancellationToken)
            .ConfigureAwait(false);
    }

    // The Redis key for these four uniform record types is a public,
    // cross-language contract (21-location-runtime.md#2.4): the Redis
    // provider hashes ZLinkStoreKey.Value with SHA-256 to build the actual
    // Redis key (ZLinkRedisLocationKeys.OpaqueRecordKey), so ZLinkStoreKey
    // .Value itself must equal the exact NUL-delimited logical key preimage
    // the spec pins -- raw UTF-8 bytes, no percent-encoding or length
    // framing. The record-kind segments below (mesh-node/owner-lease/
    // client-server/fanout-publisher) double as scan prefixes: a NUL byte
    // terminates each variable segment, so prefix scans by MeshName/
    // ChannelName below cannot spuriously match a longer name that merely
    // starts with the same characters.
    // Visibility is `internal` (not `private`) on these four key builders
    // specifically so StoreRecordGoldenTests can drive the production
    // preimage under conformance test (checklist C-3/C-4 golden fixture),
    // rather than reimplementing preimage construction in the test.
    internal static ZLinkStoreKey OwnerKey(string ownerId) =>
        Key($"owner-lease\0{ownerId}");

    internal static ZLinkStoreKey MeshKey(string meshName, RoutingId rid) =>
        Key($"{MeshPrefix(meshName)}{rid.ToHex()}");

    private static string MeshPrefix(string meshName) =>
        $"mesh-node\0{meshName}\0";

    internal static ZLinkStoreKey ClientServerKey(
        string channelName,
        RoutingId rid) =>
        Key($"{ClientServerPrefix(channelName)}{rid.ToHex()}");

    private static string ClientServerPrefix(string channelName) =>
        $"client-server\0{channelName}\0";

    internal static ZLinkStoreKey FanoutKey(
        string channelName,
        RoutingId rid) =>
        Key($"{FanoutPrefix(channelName)}{rid.ToHex()}");

    private static string FanoutPrefix(string channelName) =>
        $"fanout-publisher\0{channelName}\0";

    /// <summary>
    /// Test-only accessor (StoreRecordGoldenTests) for the canonical
    /// owner-lease JSON envelope byte encoding.
    /// </summary>
    internal static byte[] EncodeOwnerLeaseRecordForGoldenTest(
        string ownerId,
        long leaseGeneration) =>
        Encode(new OwnerRecord(ownerId, leaseGeneration));

    // Provider-private keys (capacity, reservation, aggregate, terminal,
    // generation counter) aren't part of the public opaque-record contract
    // (21-location-runtime.md#1.2) and keep this length-framed encoding.
    private static string EncodeSegment(string value) =>
        Encoding.UTF8.GetByteCount(value).ToString(
            CultureInfo.InvariantCulture) + ":" + value + ":";

    private static ZLinkStoreKey Key(string value) => new(value);

    private static T Decode<T>(ReadOnlyMemory<byte> bytes) =>
        JsonSerializer.Deserialize<T>(
            bytes.Span,
            ZLinkJsonSerializerOptions.Default)
        ?? throw new InvalidDataException(
            "The Location Store record is invalid.");

    // Descriptor records (mesh-node/client-server/fanout-publisher) check
    // recordVersion explicitly (21-location-runtime.md#2.4: "The framework,
    // not the provider, checks this value; on an unrecognized value it
    // fails explicitly instead of guessing how to read it"). The owner-lease
    // record gets the same fail-closed check through DecodeOwner below, and
    // the authority record through DecodeAuthorityMeta in the Authority
    // partial. Writers always emit recordVersion (the DTOs carry it as an
    // always-serialized init property), so validating the deserialized
    // value suffices: a value defaulted from a missing field is 1, which is
    // by construction a record this reader understands.
    private static DescriptorRecord<T> DecodeDescriptor<T>(
        ReadOnlyMemory<byte> bytes)
    {
        var record = Decode<DescriptorRecord<T>>(bytes);
        if (record.RecordVersion != 1)
            throw new InvalidDataException(
                "The Location Store descriptor record has an unrecognized"
                + " recordVersion.");
        return record;
    }

    // Owner-lease record reader (spec 21 §420-425: every opaque record
    // reader MUST fail on an unrecognized recordVersion instead of
    // guessing how to read it).
    private static OwnerRecord DecodeOwner(ReadOnlyMemory<byte> bytes)
    {
        var record = Decode<OwnerRecord>(bytes);
        if (record.RecordVersion != 1)
            throw new InvalidDataException(
                "The Location Store owner lease record has an unrecognized"
                + " recordVersion.");
        return record;
    }

    // Canonical opaque-record envelope for the owner-lease record
    // (21-location-runtime.md#2.4): recordVersion is currently always 1,
    // and leaseGeneration is a decimal-string 64-bit value per the field
    // table ("integer fields are written as JSON strings rather than JSON
    // numbers... because 64-bit values can exceed JSON number precision").
    private sealed record OwnerRecord(
        [property: JsonPropertyName("ownerId")] string OwnerId,
        [property: JsonPropertyName("leaseGeneration")]
        [property: JsonConverter(
            typeof(Messaging.ZLinkJsonSerializerOptions
                .FrameworkSigned64JsonConverter))]
        long LeaseGeneration)
    {
        [JsonPropertyName("recordVersion")]
        [JsonPropertyOrder(-1)]
        public int RecordVersion { get; init; } = 1;
    }

    // Canonical opaque-record envelope shared by the mesh-node/
    // client-server/fanout-publisher descriptors (21-location-runtime.md
    // #2.4's general table): {recordVersion, ownerId, leaseGeneration,
    // descriptorRevision, descriptor}. No top-level lifecycleGeneration and
    // no provider-internal generation counter -- both were removed by the
    // C-4f store closure; lifecycleGeneration only lives inside `descriptor`
    // now (callers read it back out via a per-T selector), and the
    // generation this repository reports to a caller is derived from the
    // provider's own opaque version instead (see VersionOf above).
    private sealed record DescriptorRecord<T>(
        string OwnerId,
        [property: JsonConverter(
            typeof(Messaging.ZLinkJsonSerializerOptions
                .FrameworkSigned64JsonConverter))]
        long LeaseGeneration,
        [property: JsonConverter(
            typeof(Messaging.ZLinkJsonSerializerOptions
                .FrameworkUnsigned64JsonConverter))]
        ulong DescriptorRevision,
        T Descriptor)
    {
        [JsonPropertyName("recordVersion")]
        [JsonPropertyOrder(-1)]
        public int RecordVersion { get; init; } = 1;
    }
}
