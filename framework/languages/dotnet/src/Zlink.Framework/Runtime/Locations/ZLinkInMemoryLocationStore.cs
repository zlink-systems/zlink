using System.Globalization;

namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// One lease per framework runtime instance. Location rows are live only
/// while their owner's lease is live.
/// </summary>
internal sealed record ZLinkOwnerLease(
    string OwnerId,
    RoutingId NodeRid,
    DateTimeOffset LeaseExpiresAt,
    DateTimeOffset UpdatedAt)
{
    public long LeaseGeneration { get; init; }
}

/// <summary>
/// Single-process store for local development, unit tests, and sample smoke
/// tests. It backs the complete location store contract in one instance,
/// which satisfies the contract requirement that location
/// rows and owner leases share one physical store. Never use it for
/// production topologies where processes must share location data.
/// </summary>
internal partial class ZLinkInMemoryLocationStore :
    IZLinkLocationRepository
{
    private readonly object _gate = new();
    private readonly TimeProvider _time;
    private readonly Dictionary<string, ZLinkOwnerLease> _leases = [];
    private long _ownerLeaseGeneration;
    private readonly RowTable<ZLinkMeshNodeDescriptor> _meshNodes = new();
    private readonly Dictionary<string, EntrySpotIdClaim> _entrySpotIdClaims =
        new(StringComparer.Ordinal);
    private readonly RowTable<ZLinkClientServerServerDescriptor> _clientServers = new();
    private readonly RowTable<ZLinkFanoutPublisherDescriptor> _fanoutPublishers = new();
    private readonly Dictionary<string, ulong> _meshNodeStamps =
        new(StringComparer.Ordinal);

    public ZLinkInMemoryLocationStore(TimeProvider? timeProvider = null)
    {
        _time = timeProvider ?? TimeProvider.System;
    }

    public ValueTask<ZLinkLocationWriteResult> UpdateMeshNodeAsync(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationWriteIntent intent,
        CancellationToken cancellationToken = default)
    {
        ValidateMeshNodeDescriptor(descriptor);
        descriptor = CanonicalizeMeshNodeDescriptor(descriptor);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var now = _time.GetUtcNow();
            var owner = new ZLinkLocationOwnerToken(
                descriptor.OwnerId,
                descriptor.LeaseGeneration);
            if (!MatchesLiveOwnerLease(owner, now))
                return ValueTask.FromResult(
                    ZLinkLocationWriteResult.IgnoredStale);

            var key = ZLinkLocationKeyCodec.EncodeMeshNodeKey(
                new ZLinkMeshNodeDescriptorKey(
                    descriptor.MeshName,
                    descriptor.Rid));
            var exists = _meshNodes.Rows.TryGetValue(key, out var current);
            var currentOwnerLive = exists
                                   && MatchesLiveOwnerLease(
                                       new ZLinkLocationOwnerToken(
                                           current!.OwnerId,
                                           current.LeaseGeneration),
                                       now);
            if (intent == ZLinkLocationWriteIntent.NewClaim && currentOwnerLive)
                return ValueTask.FromResult(
                    ZLinkLocationWriteResult.RejectedConflict);
            if (intent == ZLinkLocationWriteIntent.Takeover && currentOwnerLive)
                return ValueTask.FromResult(
                    ZLinkLocationWriteResult.IgnoredStale);
            if (intent == ZLinkLocationWriteIntent.Renew)
            {
                if (!exists
                    || current!.OwnerId != descriptor.OwnerId
                    || current.LeaseGeneration != descriptor.LeaseGeneration
                    || current.LifecycleGeneration
                    != descriptor.LifecycleGeneration
                    || !MeshNodeImmutableFieldsEqual(current, descriptor)
                    || descriptor.DescriptorRevision
                    <= current.DescriptorRevision)
                    return ValueTask.FromResult(
                        ZLinkLocationWriteResult.IgnoredStale);
            }
            if (!CanPublishEntrySpotIdNoLock(descriptor, key, now))
                return ValueTask.FromResult(
                    ZLinkLocationWriteResult.RejectedConflict);

            _meshNodes.Generations.TryGetValue(key, out var last);
            var generation = intent == ZLinkLocationWriteIntent.Renew
                ? last
                : checked(last + 1);
            _meshNodes.Generations[key] = generation;
            _meshNodes.Rows[key] = WithCurrentPlacementCapacity(
                descriptor with { UpdatedAt = now });
            PublishEntrySpotIdNoLock(current, descriptor, key);
            BumpMeshNodeStamp(descriptor.MeshName);
            return ValueTask.FromResult(
                ZLinkLocationWriteResult.Stored(generation, now));
        }
    }

    public ValueTask<ZLinkLocationWriteStatus> RemoveMeshNodeAsync(
        ZLinkMeshNodeDescriptorKey key,
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var canonicalKey = ZLinkLocationKeyCodec.EncodeMeshNodeKey(key);
        lock (_gate)
        {
            if (!_meshNodes.Rows.TryGetValue(canonicalKey, out var row)
                || row.OwnerId != owner.OwnerId
                || row.LeaseGeneration != owner.LeaseGeneration)
                return ValueTask.FromResult(
                    ZLinkLocationWriteStatus.IgnoredStale);

            _meshNodes.Rows.Remove(canonicalKey);
            RemoveEntrySpotIdClaimNoLock(row, canonicalKey);
            BumpMeshNodeStamp(key.MeshName);
            return ValueTask.FromResult(ZLinkLocationWriteStatus.Stored);
        }
    }

    public ValueTask<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> ListMeshNodesAsync(
        string meshName,
        ZLinkPageRequest page,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(meshName);
        var pageSize = ZLinkPageRequestPolicy.Normalize(page).PageSize;
        var offset = page.ContinuationToken is { } token
            && int.TryParse(token, NumberStyles.None, CultureInfo.InvariantCulture, out var parsed)
            && parsed >= 0
                ? parsed
                : page.ContinuationToken is null
                    ? 0
                    : throw new ArgumentException(
                        "The MeshNode continuation token is invalid.",
                        nameof(page));
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var rows = _meshNodes.Rows.Values
                .Where(row => string.Equals(row.MeshName, meshName, StringComparison.Ordinal))
                .Select(WithCurrentPlacementCapacity)
                .OrderBy(static row => row.Rid.ToString(), StringComparer.Ordinal)
                .ToArray();
            var items = rows.Skip(offset).Take(pageSize).ToArray();
            var nextOffset = offset + items.Length;
            return ValueTask.FromResult(
                new ZLinkLocationPage<ZLinkMeshNodeDescriptor>(
                    items,
                    nextOffset < rows.Length
                        ? nextOffset.ToString(CultureInfo.InvariantCulture)
                        : null));
        }
    }

    public ValueTask<ZLinkLocationWriteResult> UpdateClientServerAsync(
        ZLinkClientServerServerDescriptor descriptor,
        ZLinkLocationWriteIntent intent,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(descriptor.ChannelName);
        ArgumentException.ThrowIfNullOrWhiteSpace(descriptor.Endpoint);
        ArgumentException.ThrowIfNullOrWhiteSpace(descriptor.OwnerId);
        if (descriptor.ServerRid.Size == 0
            || descriptor.LifecycleGeneration == 0
            || descriptor.DescriptorRevision == 0
            || descriptor.Weight is < 0 or > ZLinkSocketConfig.MaximumPeerWeight)
            throw new ArgumentOutOfRangeException(nameof(descriptor));
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var now = _time.GetUtcNow();
            var owner = new ZLinkLocationOwnerToken(
                descriptor.OwnerId,
                descriptor.LeaseGeneration);
            if (!MatchesLiveOwnerLease(owner, now))
                return ValueTask.FromResult(ZLinkLocationWriteResult.IgnoredStale);

            var key = ClientServerKey(descriptor.ChannelName, descriptor.ServerRid);
            var exists = _clientServers.Rows.TryGetValue(key, out var current);
            var currentOwnerLive = exists
                                   && MatchesLiveOwnerLease(
                                       new ZLinkLocationOwnerToken(
                                           current!.OwnerId,
                                           current.LeaseGeneration),
                                       now);
            if (intent == ZLinkLocationWriteIntent.NewClaim && currentOwnerLive)
                return ValueTask.FromResult(ZLinkLocationWriteResult.RejectedConflict);
            if (intent == ZLinkLocationWriteIntent.Takeover && currentOwnerLive)
                return ValueTask.FromResult(ZLinkLocationWriteResult.IgnoredStale);
            if (intent == ZLinkLocationWriteIntent.Renew
                && (!exists
                    || current!.OwnerId != descriptor.OwnerId
                    || current.LeaseGeneration != descriptor.LeaseGeneration
                    || current.LifecycleGeneration != descriptor.LifecycleGeneration
                    || current.Endpoint != descriptor.Endpoint
                    || current.SecurityIdentity != descriptor.SecurityIdentity
                    || descriptor.DescriptorRevision <= current.DescriptorRevision))
                return ValueTask.FromResult(ZLinkLocationWriteResult.IgnoredStale);

            _clientServers.Generations.TryGetValue(key, out var last);
            var generation = intent == ZLinkLocationWriteIntent.Renew
                ? last
                : checked(last + 1);
            _clientServers.Generations[key] = generation;
            _clientServers.Rows[key] = descriptor with { UpdatedAt = now };
            return ValueTask.FromResult(ZLinkLocationWriteResult.Stored(generation, now));
        }
    }

    public ValueTask<ZLinkLocationWriteStatus> RemoveClientServerAsync(
        ZLinkClientServerServerDescriptorKey key,
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var encoded = ClientServerKey(key.ChannelName, key.ServerRid);
            if (!_clientServers.Rows.TryGetValue(encoded, out var current)
                || current.OwnerId != owner.OwnerId
                || current.LeaseGeneration != owner.LeaseGeneration)
                return ValueTask.FromResult(ZLinkLocationWriteStatus.IgnoredStale);
            _clientServers.Rows.Remove(encoded);
            return ValueTask.FromResult(ZLinkLocationWriteStatus.Stored);
        }
    }

    public ValueTask<ZLinkLocationPage<ZLinkClientServerServerDescriptor>>
        ListClientServersAsync(
            string channelName,
            ZLinkPageRequest page,
            CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var pageSize = ZLinkPageRequestPolicy.Normalize(page).PageSize;
            var offset = page.ContinuationToken is { } token
                         && int.TryParse(token, out var parsed)
                ? parsed
                : 0;
            var rows = _clientServers.Rows.Values
                .Where(row => StringComparer.Ordinal.Equals(
                    row.ChannelName,
                    channelName))
                .OrderBy(static row => row.ServerRid.ToHex(), StringComparer.Ordinal)
                .ToArray();
            var items = rows.Skip(offset).Take(pageSize).ToArray();
            var next = offset + items.Length < rows.Length
                ? (offset + items.Length).ToString(CultureInfo.InvariantCulture)
                : null;
            return ValueTask.FromResult(
                new ZLinkLocationPage<ZLinkClientServerServerDescriptor>(
                    items,
                    next));
        }
    }

    private static string ClientServerKey(string channelName, RoutingId rid) =>
        $"{channelName}\u001f{rid.ToHex()}";

    public ValueTask<ZLinkLocationWriteResult> UpdateFanoutPublisherAsync(
        ZLinkFanoutPublisherDescriptor descriptor,
        ZLinkLocationWriteIntent intent,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(descriptor.ChannelName);
        ArgumentException.ThrowIfNullOrWhiteSpace(descriptor.Endpoint);
        ArgumentException.ThrowIfNullOrWhiteSpace(descriptor.OwnerId);
        if (descriptor.PublisherRid.Size == 0
            || descriptor.LifecycleGeneration == 0
            || descriptor.DescriptorRevision == 0)
            throw new ArgumentOutOfRangeException(nameof(descriptor));
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var now = _time.GetUtcNow();
            var owner = new ZLinkLocationOwnerToken(
                descriptor.OwnerId,
                descriptor.LeaseGeneration);
            if (!MatchesLiveOwnerLease(owner, now))
                return ValueTask.FromResult(ZLinkLocationWriteResult.IgnoredStale);

            var key = FanoutKey(descriptor.ChannelName, descriptor.PublisherRid);
            var exists = _fanoutPublishers.Rows.TryGetValue(key, out var current);
            var currentOwnerLive = exists
                                   && MatchesLiveOwnerLease(
                                       new ZLinkLocationOwnerToken(
                                           current!.OwnerId,
                                           current.LeaseGeneration),
                                       now);
            if (intent == ZLinkLocationWriteIntent.NewClaim && currentOwnerLive)
                return ValueTask.FromResult(ZLinkLocationWriteResult.RejectedConflict);
            if (intent == ZLinkLocationWriteIntent.Takeover && currentOwnerLive)
                return ValueTask.FromResult(ZLinkLocationWriteResult.IgnoredStale);
            if (intent == ZLinkLocationWriteIntent.Renew
                && (!exists
                    || current!.OwnerId != descriptor.OwnerId
                    || current.LeaseGeneration != descriptor.LeaseGeneration
                    || current.LifecycleGeneration != descriptor.LifecycleGeneration
                    || current.Endpoint != descriptor.Endpoint
                    || current.SecurityIdentity != descriptor.SecurityIdentity
                    || descriptor.DescriptorRevision <= current.DescriptorRevision))
                return ValueTask.FromResult(ZLinkLocationWriteResult.IgnoredStale);

            _fanoutPublishers.Generations.TryGetValue(key, out var last);
            var generation = intent == ZLinkLocationWriteIntent.Renew
                ? last
                : checked(last + 1);
            _fanoutPublishers.Generations[key] = generation;
            _fanoutPublishers.Rows[key] = descriptor with { UpdatedAt = now };
            return ValueTask.FromResult(ZLinkLocationWriteResult.Stored(generation, now));
        }
    }

    public ValueTask<ZLinkLocationWriteStatus> RemoveFanoutPublisherAsync(
        ZLinkFanoutPublisherDescriptorKey key,
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var encoded = FanoutKey(key.ChannelName, key.PublisherRid);
            if (!_fanoutPublishers.Rows.TryGetValue(encoded, out var current)
                || current.OwnerId != owner.OwnerId
                || current.LeaseGeneration != owner.LeaseGeneration)
                return ValueTask.FromResult(ZLinkLocationWriteStatus.IgnoredStale);
            _fanoutPublishers.Rows.Remove(encoded);
            return ValueTask.FromResult(ZLinkLocationWriteStatus.Stored);
        }
    }

    public ValueTask<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>>
        ListFanoutPublishersAsync(
            string channelName,
            ZLinkPageRequest page,
            CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var pageSize = ZLinkPageRequestPolicy.Normalize(page).PageSize;
            var offset = page.ContinuationToken is { } token
                         && int.TryParse(token, out var parsed)
                ? parsed
                : 0;
            var rows = _fanoutPublishers.Rows.Values
                .Where(row => StringComparer.Ordinal.Equals(
                    row.ChannelName,
                    channelName))
                .OrderBy(static row => row.PublisherRid.ToHex(), StringComparer.Ordinal)
                .ToArray();
            var items = rows.Skip(offset).Take(pageSize).ToArray();
            var next = offset + items.Length < rows.Length
                ? (offset + items.Length).ToString(CultureInfo.InvariantCulture)
                : null;
            return ValueTask.FromResult(
                new ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>(
                    items,
                    next));
        }
    }

    private static string FanoutKey(string channelName, RoutingId rid) =>
        $"{channelName}\u001f{rid.ToHex()}";

    private ZLinkMeshNodeDescriptor WithCurrentPlacementCapacity(
        ZLinkMeshNodeDescriptor descriptor)
    {
        var key = new ZLinkMeshNodeDescriptorKey(
            descriptor.MeshName,
            descriptor.Rid);
        var actorActive = PlacementCapacityUsage(
            _activePlacementCapacity,
            key,
            descriptor.LifecycleGeneration,
            ZLinkPlacementObjectKind.Actor);
        var actorReserved = PlacementCapacityUsage(
            _pendingPlacementCapacity,
            key,
            descriptor.LifecycleGeneration,
            ZLinkPlacementObjectKind.Actor);
        var spotActive = PlacementCapacityUsage(
            _activePlacementCapacity,
            key,
            descriptor.LifecycleGeneration,
            ZLinkPlacementObjectKind.UserSpot,
            ZLinkPlacementObjectKind.InstanceSpot);
        var spotReserved = PlacementCapacityUsage(
            _pendingPlacementCapacity,
            key,
            descriptor.LifecycleGeneration,
            ZLinkPlacementObjectKind.UserSpot,
            ZLinkPlacementObjectKind.InstanceSpot);
        var spotTypes = descriptor.ObjectCapabilities
            .Where(static capability =>
                capability.ObjectKind is ZLinkPlacementObjectKind.UserSpot
                    or ZLinkPlacementObjectKind.InstanceSpot)
            .Select(capability =>
            {
                var typeKey = new PlacementCapacityKey(
                    key,
                    descriptor.LifecycleGeneration,
                    capability.ObjectKind,
                    capability.StableType);
                return new ZLinkSpotTypeCapacity(
                    capability.ObjectKind,
                    capability.StableType,
                    checked((int)_activePlacementCapacity
                        .GetValueOrDefault(typeKey)),
                    checked((int)_pendingPlacementCapacity
                        .GetValueOrDefault(typeKey)),
                    capability.Limit);
            })
            .ToArray();
        return descriptor with
        {
            Capacity = new ZLinkPlacementCapacity(
                descriptor.Capacity.Actors with
                {
                    Active = checked((int)actorActive),
                    Reserved = checked((int)actorReserved)
                },
                descriptor.Capacity.Spots with
                {
                    Active = checked((int)spotActive),
                    Reserved = checked((int)spotReserved)
                },
                spotTypes)
        };
    }

    private static bool MeshNodeImmutableFieldsEqual(
        ZLinkMeshNodeDescriptor current,
        ZLinkMeshNodeDescriptor incoming) =>
        current.MeshName == incoming.MeshName
        && current.Rid == incoming.Rid
        && current.LifecycleGeneration == incoming.LifecycleGeneration
        && (current.Endpoint == incoming.Endpoint
            || current.State == ZLinkFrameworkRuntimeState.Preparing
            && current.Endpoint.Length == 0
            && incoming.Endpoint.Length > 0)
        && current.SecurityIdentity == incoming.SecurityIdentity
        && current.OwnerId == incoming.OwnerId
        && current.LeaseGeneration == incoming.LeaseGeneration
        && current.ApplicationVersion == incoming.ApplicationVersion
        && current.ObjectRole == incoming.ObjectRole
        && current.ChannelWeights.Keys.ToHashSet(StringComparer.Ordinal)
            .SetEquals(incoming.ChannelWeights.Keys)
        && ObjectCapabilitiesEqual(
            current.ObjectCapabilities,
            incoming.ObjectCapabilities)
        && current.Capacity.Actors.Limit == incoming.Capacity.Actors.Limit
        && current.Capacity.Spots.Limit == incoming.Capacity.Spots.Limit;


    private static ZLinkMeshNodeDescriptor CanonicalizeMeshNodeDescriptor(
        ZLinkMeshNodeDescriptor descriptor) =>
        descriptor with
        {
            ChannelWeights = descriptor.ChannelWeights
                .OrderBy(
                    static pair => pair.Key,
                    Utf8StringComparer.Instance)
                .ToDictionary(
                    static pair => pair.Key,
                    static pair => pair.Value,
                    StringComparer.Ordinal),
            ObjectCapabilities = descriptor.ObjectCapabilities
                .OrderBy(static capability => capability.ObjectKind)
                .ThenBy(
                    static capability => capability.StableType,
                    Utf8StringComparer.Instance)
                .ToArray()
        };

    private static bool ObjectCapabilitiesEqual(
        IReadOnlyList<ZLinkObjectCapability> current,
        IReadOnlyList<ZLinkObjectCapability> incoming)
    {
        if (current.Count != incoming.Count)
            return false;
        for (var index = 0; index < current.Count; index++)
        {
            var left = current[index];
            var right = incoming[index];
            if (left.ObjectKind != right.ObjectKind
                || left.StableType != right.StableType
                || left.Policy != right.Policy
                || left.HasSnapshotAdapter != right.HasSnapshotAdapter
                || left.Limit != right.Limit)
                return false;
        }
        return true;
    }

    private static void ValidateMeshNodeDescriptor(
        ZLinkMeshNodeDescriptor descriptor)
    {
        ArgumentNullException.ThrowIfNull(descriptor);
        ValidateUtf8Value(descriptor.MeshName, nameof(descriptor.MeshName));
        if (descriptor.Rid.IsEmpty
            || descriptor.LifecycleGeneration == 0
            || descriptor.DescriptorRevision == 0
            || descriptor.ApplicationVersion < 0
            || descriptor.ChannelWeights is null
            || string.IsNullOrWhiteSpace(descriptor.OwnerId)
            || descriptor.LeaseGeneration <= 0
            || !Enum.IsDefined(descriptor.State)
            || !Enum.IsDefined(descriptor.ObjectRole)
            || descriptor.PlacementWeight is < 0 or > ZLinkSocketConfig.MaximumPeerWeight
            || !IsValidCapacity(descriptor.Capacity.Actors)
            || !IsValidCapacity(descriptor.Capacity.Spots)
            || descriptor.Capacity.SpotTypes is null
            || descriptor.ActivationConcurrency is not
            {
                Active: >= 0,
                Limit: > 0
            }
            || descriptor.ActivationConcurrency.Active
                > descriptor.ActivationConcurrency.Limit
            || descriptor.ObjectCapabilities is null
            || descriptor.ObjectCapabilities.Count > 1024
            || descriptor.ObjectRole != ZLinkMeshNodeObjectRole.Server
            && descriptor.ObjectCapabilities.Count != 0)
            throw new ArgumentException(
                "The MeshNode descriptor is invalid.",
                nameof(descriptor));
        foreach (var (channelName, weight) in descriptor.ChannelWeights)
        {
            ValidateUtf8Value(channelName, "ChannelName");
            if (weight is < 0 or > ZLinkSocketConfig.MaximumPeerWeight)
                throw new ArgumentOutOfRangeException(
                    nameof(descriptor),
                    "Channel weight must be between 0 and 10000.");
        }
        if (descriptor.MaintenanceWave is { } wave)
            ValidateUtf8Value(wave, nameof(descriptor.MaintenanceWave));
        if (descriptor.ObjectRole == ZLinkMeshNodeObjectRole.Server)
            ValidateUtf8Value(
                descriptor.EntrySpotId
                ?? throw new ArgumentException(
                    "Object Server descriptor EntrySpotId is required.",
                    nameof(descriptor)),
                nameof(descriptor.EntrySpotId));
        else if (descriptor.EntrySpotId is not null)
            throw new ArgumentException(
                "Only an Object Server descriptor can publish EntrySpotId.",
                nameof(descriptor));
        var identities =
            new HashSet<(ZLinkPlacementObjectKind, string)>();
        foreach (var capability in descriptor.ObjectCapabilities)
        {
            if (capability is null
                || !Enum.IsDefined(capability.ObjectKind)
                || !Enum.IsDefined(capability.Policy)
                || !identities.Add((
                    capability.ObjectKind,
                    capability.StableType))
                || capability.Policy
                == ZLinkObjectMaintenancePolicyKind.Snapshot
                != capability.HasSnapshotAdapter
                || capability.Limit < 0
                || capability.ObjectKind == ZLinkPlacementObjectKind.Actor
                    && capability.Limit != 0)
                throw new ArgumentException(
                    "The MeshNode object capabilities are invalid.",
                    nameof(descriptor));
            ValidateUtf8Value(
                capability.StableType,
                nameof(capability.StableType));
        }
        var expectedSpotTypes = descriptor.ObjectCapabilities
            .Where(static capability =>
                capability.ObjectKind is ZLinkPlacementObjectKind.UserSpot
                    or ZLinkPlacementObjectKind.InstanceSpot)
            .Select(static capability =>
                (capability.ObjectKind, capability.StableType, capability.Limit))
            .ToArray();
        if (descriptor.Capacity.SpotTypes.Count != expectedSpotTypes.Length)
            throw new ArgumentException(
                "The MeshNode Spot type capacity projection is invalid.",
                nameof(descriptor));
        for (var index = 0; index < expectedSpotTypes.Length; index++)
        {
            var capacity = descriptor.Capacity.SpotTypes[index];
            var expected = expectedSpotTypes[index];
            if (capacity.ObjectKind != expected.ObjectKind
                || capacity.StableType != expected.StableType
                || capacity.Limit != expected.Limit
                || capacity.Active < 0
                || capacity.Reserved < 0
                || capacity.Limit > 0
                    && capacity.Active + (long)capacity.Reserved
                    > capacity.Limit)
                throw new ArgumentException(
                    "The MeshNode Spot type capacity projection is invalid.",
                    nameof(descriptor));
        }
    }

    private static bool IsValidCapacity(ZLinkPopulationCapacity capacity) =>
        capacity is { Active: >= 0, Reserved: >= 0, Limit: >= 0 }
        && (capacity.Limit == 0
            || capacity.Active + (long)capacity.Reserved <= capacity.Limit);

    private bool CanPublishEntrySpotIdNoLock(
        ZLinkMeshNodeDescriptor descriptor,
        string descriptorKey,
        DateTimeOffset now)
    {
        if (descriptor.EntrySpotId is not { } entrySpotId)
            return true;

        if (_entrySpotIdClaims.TryGetValue(entrySpotId, out var claim)
            && (!string.Equals(
                    claim.DescriptorKey,
                    descriptorKey,
                    StringComparison.Ordinal)
                || claim.DescriptorLifecycleGeneration
                != descriptor.LifecycleGeneration
                || claim.Owner != new ZLinkLocationOwnerToken(
                    descriptor.OwnerId,
                    descriptor.LeaseGeneration))
            && MatchesLiveOwnerLease(claim.Owner, now))
            return false;

        var authorityKey =
            Zlink.Framework.Runtime.Spots.ZLinkUserSpotAuthorityPayloadCodec
                .AuthorityKey(entrySpotId);
        if (_authorities.ContainsKey(authorityKey.Value))
            return false;
        return true;
    }

    private void PublishEntrySpotIdNoLock(
        ZLinkMeshNodeDescriptor? previous,
        ZLinkMeshNodeDescriptor descriptor,
        string descriptorKey)
    {
        if (previous is not null
            && !string.Equals(
                previous.EntrySpotId,
                descriptor.EntrySpotId,
                StringComparison.Ordinal))
            RemoveEntrySpotIdClaimNoLock(previous, descriptorKey);

        if (descriptor.EntrySpotId is { } entrySpotId)
        {
            _entrySpotIdClaims[entrySpotId] = new EntrySpotIdClaim(
                descriptorKey,
                descriptor.LifecycleGeneration,
                new ZLinkLocationOwnerToken(
                    descriptor.OwnerId,
                    descriptor.LeaseGeneration));
        }
    }

    private void RemoveEntrySpotIdClaimNoLock(
        ZLinkMeshNodeDescriptor descriptor,
        string descriptorKey)
    {
        if (descriptor.EntrySpotId is not { } entrySpotId
            || !_entrySpotIdClaims.TryGetValue(entrySpotId, out var claim)
            || !string.Equals(
                claim.DescriptorKey,
                descriptorKey,
                StringComparison.Ordinal)
            || claim.DescriptorLifecycleGeneration
            != descriptor.LifecycleGeneration
            || claim.Owner != new ZLinkLocationOwnerToken(
                descriptor.OwnerId,
                descriptor.LeaseGeneration))
            return;

        _entrySpotIdClaims.Remove(entrySpotId);
    }

    private static void ValidateUtf8Value(string value, string name)
    {
        var size = System.Text.Encoding.UTF8.GetByteCount(value);
        if (size is < 1 or > 255 || value.Contains('\0'))
            throw new ArgumentException(
                $"{name} must be 1 to 255 UTF-8 bytes without NUL.",
                name);
    }

    private sealed class Utf8StringComparer : IComparer<string>
    {
        internal static Utf8StringComparer Instance { get; } = new();

        public int Compare(string? left, string? right)
        {
            if (ReferenceEquals(left, right))
                return 0;
            if (left is null)
                return -1;
            if (right is null)
                return 1;
            return System.Text.Encoding.UTF8.GetBytes(left)
                .AsSpan()
                .SequenceCompareTo(
                    System.Text.Encoding.UTF8.GetBytes(right));
        }
    }

    private sealed record EntrySpotIdClaim(
        string DescriptorKey,
        ulong DescriptorLifecycleGeneration,
        ZLinkLocationOwnerToken Owner);

    public ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
        string ownerId,
        TimeSpan leaseTtl,
        CancellationToken cancellationToken = default)
    {
        ValidateOwnerLeaseArguments(ownerId, leaseTtl);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var now = _time.GetUtcNow();
            if (_leases.TryGetValue(ownerId, out var current)
                && current.LeaseExpiresAt > now)
                return ValueTask.FromResult<ZLinkOwnerLeaseClaimResult>(
                    new ZLinkOwnerLeaseClaimResult.Conflict());
            if (_ownerLeaseGeneration == long.MaxValue)
                return ValueTask.FromResult<ZLinkOwnerLeaseClaimResult>(
                    new ZLinkOwnerLeaseClaimResult.GenerationExhausted());

            var token = new ZLinkLocationOwnerToken(
                ownerId,
                ++_ownerLeaseGeneration);
            var expiresAt = now + leaseTtl;
            _leases[ownerId] = new ZLinkOwnerLease(
                ownerId,
                default,
                expiresAt,
                now)
            {
                LeaseGeneration = token.LeaseGeneration
            };
            return ValueTask.FromResult<ZLinkOwnerLeaseClaimResult>(
                new ZLinkOwnerLeaseClaimResult.Claimed(
                    token,
                    expiresAt,
                    now));
        }
    }

    public ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
        string ownerId,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(ownerId);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var now = _time.GetUtcNow();
            if (!_leases.TryGetValue(ownerId, out var lease)
                || lease.LeaseExpiresAt <= now)
                return ValueTask.FromResult<ZLinkOwnerLeaseReadResult>(
                    new ZLinkOwnerLeaseReadResult.Missing());
            return ValueTask.FromResult<ZLinkOwnerLeaseReadResult>(
                new ZLinkOwnerLeaseReadResult.Found(
                    new ZLinkLocationOwnerToken(
                        ownerId,
                        lease.LeaseGeneration),
                    lease.LeaseExpiresAt,
                    now));
        }
    }

    public ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
        ZLinkLocationOwnerToken token,
        TimeSpan leaseTtl,
        CancellationToken cancellationToken = default)
    {
        ValidateOwnerLeaseArguments(token.OwnerId, leaseTtl);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            var now = _time.GetUtcNow();
            if (!MatchesLiveOwnerLease(token, now))
                return ValueTask.FromResult<ZLinkOwnerLeaseRenewResult>(
                    new ZLinkOwnerLeaseRenewResult.Stale());
            var current = _leases[token.OwnerId];
            var expiresAt = now + leaseTtl;
            _leases[token.OwnerId] = current with
            {
                LeaseExpiresAt = expiresAt,
                UpdatedAt = now
            };
            return ValueTask.FromResult<ZLinkOwnerLeaseRenewResult>(
                new ZLinkOwnerLeaseRenewResult.Renewed(expiresAt, now));
        }
    }

    public ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
        ZLinkLocationOwnerToken token,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(token.OwnerId);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            if (!MatchesLiveOwnerLease(token, _time.GetUtcNow()))
                return ValueTask.FromResult(ZLinkOwnerLeaseReleaseResult.Stale);
            _leases.Remove(token.OwnerId);
            return ValueTask.FromResult(ZLinkOwnerLeaseReleaseResult.Released);
        }
    }

    public ValueTask<long> RemoveAllByOwnerAsync(
        ZLinkLocationOwnerToken owner,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(owner.OwnerId);
        if (owner.LeaseGeneration <= 0)
            throw new ArgumentOutOfRangeException(nameof(owner));
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            if (!MatchesLiveOwnerLease(owner, _time.GetUtcNow()))
                return ValueTask.FromResult(0L);
            var removed = 0L;
            var ownedDescriptors = _meshNodes.Rows
                .Where(pair => pair.Value.OwnerId == owner.OwnerId)
                .ToArray();
            foreach (var descriptor in ownedDescriptors)
            {
                _meshNodes.Rows.Remove(descriptor.Key);
                BumpMeshNodeStamp(descriptor.Value.MeshName);
                removed++;
            }
            foreach (var descriptor in ownedDescriptors)
                RemoveEntrySpotIdClaimNoLock(
                    descriptor.Value,
                    descriptor.Key);
            var clientServerKeys = _clientServers.Rows
                .Where(pair => pair.Value.OwnerId == owner.OwnerId)
                .Select(static pair => pair.Key)
                .ToArray();
            foreach (var key in clientServerKeys)
            {
                _clientServers.Rows.Remove(key);
                removed++;
            }
            var fanoutKeys = _fanoutPublishers.Rows
                .Where(pair => pair.Value.OwnerId == owner.OwnerId)
                .Select(static pair => pair.Key)
                .ToArray();
            foreach (var key in fanoutKeys)
            {
                _fanoutPublishers.Rows.Remove(key);
                removed++;
            }
            return ValueTask.FromResult(removed);
        }
    }

    public ValueTask<ulong?> GetMeshNodeChangeStampAsync(
        string meshName,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(meshName);
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            _meshNodeStamps.TryGetValue(meshName, out var stamp);
            return ValueTask.FromResult<ulong?>(stamp);
        }
    }

    private bool IsOwnerLive(string ownerId, DateTimeOffset now) =>
        _leases.TryGetValue(ownerId, out var lease) && lease.LeaseExpiresAt > now;

    private bool MatchesLiveOwnerLease(
        ZLinkLocationOwnerToken token,
        DateTimeOffset now) =>
        _leases.TryGetValue(token.OwnerId, out var lease)
        && lease.LeaseExpiresAt > now
        && lease.LeaseGeneration == token.LeaseGeneration;

    private long NextOwnerLeaseGeneration()
    {
        if (_ownerLeaseGeneration == long.MaxValue)
            throw new InvalidOperationException(
                "The owner lease generation space was exhausted.");
        return ++_ownerLeaseGeneration;
    }

    private static void ValidateOwnerLeaseArguments(
        string ownerId,
        TimeSpan leaseTtl)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(ownerId);
        if (leaseTtl <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(leaseTtl));
    }

    private void BumpMeshNodeStamp(string meshName)
    {
        _meshNodeStamps.TryGetValue(meshName, out var stamp);
        _meshNodeStamps[meshName] = stamp + 1;
    }

    private sealed class RowTable<TRow>
        where TRow : class
    {
        public Dictionary<string, TRow> Rows { get; } = [];

        public Dictionary<string, ulong> Generations { get; } = [];
    }

}
