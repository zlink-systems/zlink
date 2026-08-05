using System.Globalization;
using System.Runtime.ExceptionServices;
using System.Text;
using System.Text.Json;

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
                    long.Parse(
                        Encoding.UTF8.GetString(found.Value.Bytes.Span),
                        NumberStyles.None,
                        CultureInfo.InvariantCulture),
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
                        var record = Decode<OwnerRecord>(found.Value.Bytes);
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
        var record = Decode<OwnerRecord>(found.Bytes);
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
        if (Decode<OwnerRecord>(found.Value.Bytes).LeaseGeneration
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
        if (Decode<OwnerRecord>(found.Value.Bytes).LeaseGeneration
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
        if (Decode<OwnerRecord>(liveLease.Value.Bytes).LeaseGeneration
            != descriptor.LeaseGeneration)
            return ZLinkLocationWriteResult.IgnoredStale;

        var current = await provider.ReadAsync(rowKey, cancellationToken)
            .ConfigureAwait(false);
        ulong generation = 1;
        ZLinkStoreCondition rowCondition;
        if (current is ZLinkStoreReadResult.Found found)
        {
            var record = Decode<MeshRecord>(found.Value.Bytes);
            generation = record.Generation;
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
                generation = checked(generation + 1);
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
            new MeshRecord(
                generation,
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
                generation,
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
        var descriptor = Decode<MeshRecord>(found.Value.Bytes).Descriptor;
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
                static bytes => Decode<MeshRecord>(bytes).Descriptor,
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
                $"{Prefix}mesh:",
                owner,
                static bytes =>
                {
                    var descriptor = Decode<MeshRecord>(bytes).Descriptor;
                    return new ZLinkLocationOwnerToken(
                        descriptor.OwnerId,
                        descriptor.LeaseGeneration);
                },
                cancellationToken)
            .ConfigureAwait(false);
        removed += await RemoveOwnedDescriptorsAsync(
                $"{Prefix}client-server:",
                owner,
                static bytes =>
                {
                    var descriptor = Decode<
                        DescriptorRecord<ZLinkClientServerServerDescriptor>>(
                            bytes).Descriptor;
                    return new ZLinkLocationOwnerToken(
                        descriptor.OwnerId,
                        descriptor.LeaseGeneration);
                },
                cancellationToken)
            .ConfigureAwait(false);
        removed += await RemoveOwnedDescriptorsAsync(
                $"{Prefix}fanout:",
                owner,
                static bytes =>
                {
                    var descriptor = Decode<
                        DescriptorRecord<ZLinkFanoutPublisherDescriptor>>(
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
        Func<T, T, bool> immutableFieldsEqual,
        CancellationToken cancellationToken)
    {
        var leaseKey = OwnerKey(ownerId);
        var lease = await provider.ReadAsync(leaseKey, cancellationToken)
            .ConfigureAwait(false);
        if (lease is not ZLinkStoreReadResult.Found liveLease
            || Decode<OwnerRecord>(liveLease.Value.Bytes).LeaseGeneration
            != leaseGeneration)
            return ZLinkLocationWriteResult.IgnoredStale;

        var current = await provider.ReadAsync(rowKey, cancellationToken)
            .ConfigureAwait(false);
        ulong generation = 1;
        ZLinkStoreCondition rowCondition;
        if (current is ZLinkStoreReadResult.Found found)
        {
            var record = Decode<DescriptorRecord<T>>(found.Value.Bytes);
            generation = record.Generation;
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
                    || record.LifecycleGeneration != lifecycleGeneration
                    || descriptorRevision <= record.DescriptorRevision
                    || !immutableFieldsEqual(record.Descriptor, descriptor)))
                return ZLinkLocationWriteResult.IgnoredStale;
            if (intent != ZLinkLocationWriteIntent.Renew)
                generation = checked(generation + 1);
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
                generation,
                ownerId,
                leaseGeneration,
                lifecycleGeneration,
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
                generation,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask<ZLinkLocationWriteResult>
        WriteDescriptorWithReconciliationAsync(
            ZLinkStoreKey rowKey,
            ZLinkStoreWriteRequest request,
            ReadOnlyMemory<byte> encodedDescriptor,
            ulong generation,
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
                        generation,
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
            ? ZLinkLocationWriteResult.Stored(generation, applied.StoreNow)
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
        var record = Decode<DescriptorRecord<T>>(found.Value.Bytes);
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
                static bytes => Decode<DescriptorRecord<T>>(bytes).Descriptor,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private static ZLinkStoreKey OwnerKey(string ownerId) =>
        Key($"{Prefix}owner:{EncodeSegment(ownerId)}");

    private static ZLinkStoreKey MeshKey(string meshName, RoutingId rid) =>
        Key($"{MeshPrefix(meshName)}{EncodeSegment(rid.ToHex())}");

    private static string MeshPrefix(string meshName) =>
        $"{Prefix}mesh:{EncodeSegment(meshName)}";

    private static ZLinkStoreKey ClientServerKey(
        string channelName,
        RoutingId rid) =>
        Key($"{ClientServerPrefix(channelName)}{EncodeSegment(rid.ToHex())}");

    private static string ClientServerPrefix(string channelName) =>
        $"{Prefix}client-server:{EncodeSegment(channelName)}";

    private static ZLinkStoreKey FanoutKey(
        string channelName,
        RoutingId rid) =>
        Key($"{FanoutPrefix(channelName)}{EncodeSegment(rid.ToHex())}");

    private static string FanoutPrefix(string channelName) =>
        $"{Prefix}fanout:{EncodeSegment(channelName)}";

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

    private sealed record OwnerRecord(
        string OwnerId,
        long LeaseGeneration);

    private sealed record MeshRecord(
        ulong Generation,
        ZLinkMeshNodeDescriptor Descriptor);

    private sealed record DescriptorRecord<T>(
        ulong Generation,
        string OwnerId,
        long LeaseGeneration,
        ulong LifecycleGeneration,
        ulong DescriptorRevision,
        T Descriptor);
}
