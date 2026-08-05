using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class InMemoryLocationStoreTests
{
    private const string OwnerA = "owner-a";
    private const string OwnerB = "owner-b";
    private static readonly TimeSpan LeaseTtl = TimeSpan.FromSeconds(15);

    [Fact]
    public async Task Authority_Reserve_Issues_Monotonic_Generations_And_Rejects_Live_Key()
    {
        var (store, _) = await CreateStoreWithLiveOwnersAsync(OwnerA, OwnerB);

        var first = await CreateAuthorityAsync(store, OwnerA, "actor-1");
        Assert.Equal(1UL, first.ObjectGeneration);
        var existing = Assert.IsType<ZLinkObjectReserveResult.AlreadyExists>(
            await ReserveAuthorityAsync(store, OwnerB, "actor-1"));
        Assert.Equal(first.StoreVersion, existing.Current.StoreVersion);

        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Deleted>(
            await store.CompareExchangeAuthorityAsync(
                AuthorityKey("actor-1"),
                first.StoreVersion,
                new ZLinkAuthorityMutation.Delete()));

        // Generation counters survive removal so a re-claim can never reuse
        // an old fencing token.
        var reclaimed = await CreateAuthorityAsync(store, OwnerB, "actor-1");
        Assert.True(reclaimed.ObjectGeneration > first.ObjectGeneration);
    }

    [Fact]
    public async Task Authority_Does_Not_Silently_Replace_An_Expired_Owner()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        await store.ClaimLiveOwnerAsync(OwnerA, LeaseTtl);
        await store.ClaimLiveOwnerAsync(OwnerB, TimeSpan.FromMinutes(5));

        var first = await CreateAuthorityAsync(store, OwnerA, "actor-1");

        // Owner A stops heartbeating; its lease expires and its rows become
        // claimable without any row write.
        time.Advance(LeaseTtl + TimeSpan.FromSeconds(1));

        var existing = Assert.IsType<ZLinkObjectReserveResult.AlreadyExists>(
            await ReserveAuthorityAsync(store, OwnerB, "actor-1"));
        Assert.Equal(first.StoreVersion, existing.Current.StoreVersion);
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Conflict>(
            await store.CompareExchangeAuthorityAsync(
                AuthorityKey("actor-1"),
                first.StoreVersion,
                new ZLinkAuthorityMutation.Delete()));
    }

    [Fact]
    public async Task Authority_Preserve_Requires_Exact_Version_And_Keeps_Generations()
    {
        var (store, _) = await CreateStoreWithLiveOwnersAsync(OwnerA, OwnerB);
        var claimed = await CreateAuthorityAsync(store, OwnerA, "actor-1");

        var stored = Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await store.CompareExchangeAuthorityAsync(
                AuthorityKey("actor-1"),
                claimed.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    new byte[] { 0x33 },
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null)));
        Assert.Equal(claimed.ObjectGeneration, stored.Snapshot.ObjectGeneration);
        Assert.Equal(
            claimed.AuthorityOwnerGeneration,
            stored.Snapshot.AuthorityOwnerGeneration);
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Conflict>(
            await store.CompareExchangeAuthorityAsync(
                AuthorityKey("actor-1"),
                claimed.StoreVersion,
                new ZLinkAuthorityMutation.Delete()));
    }

    [Fact]
    public async Task Recreated_Authority_Fences_The_Previous_Version()
    {
        var (store, _) = await CreateStoreWithLiveOwnersAsync(OwnerA, OwnerB);
        var claimed = await CreateAuthorityAsync(store, OwnerA, "actor-1");
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Deleted>(
            await store.CompareExchangeAuthorityAsync(
                AuthorityKey("actor-1"),
                claimed.StoreVersion,
                new ZLinkAuthorityMutation.Delete()));
        var recreated = await CreateAuthorityAsync(store, OwnerB, "actor-1");

        Assert.True(recreated.ObjectGeneration > claimed.ObjectGeneration);
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Conflict>(
            await store.CompareExchangeAuthorityAsync(
                AuthorityKey("actor-1"),
                claimed.StoreVersion,
                new ZLinkAuthorityMutation.Delete()));
    }

    [Fact]
    public async Task RemoveByOwner_Bulk_Removes_Only_Exact_Owner_Rows()
    {
        var (store, _) = await CreateStoreWithLiveOwnersAsync(OwnerA, OwnerB);
        await CreateAuthorityAsync(store, OwnerA, "actor-1");
        await CreateAuthorityAsync(store, OwnerA, "actor-2");
        await CreateAuthorityAsync(store, OwnerB, "actor-3");

        var ownerAToken = Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await store.ReadOwnerLeaseAsync(OwnerA)).Token;
        Assert.Equal(
            0,
            await store.RemoveAllByOwnerAsync(
                ownerAToken with
                {
                    LeaseGeneration = ownerAToken.LeaseGeneration + 1
                }));
        var removed = await store.RemoveAllByOwnerAsync(ownerAToken);

        Assert.Equal(1, removed);
        Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(AuthorityKey("actor-1")));
        Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(AuthorityKey("actor-2")));
        Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(AuthorityKey("actor-3")));
    }

    [Fact]
    public async Task RemoveAllByOwner_Removes_All_Kinds_For_The_Owner()
    {
        var (store, _) = await CreateStoreWithLiveOwnersAsync(OwnerA, OwnerB);
        var descriptorA = MeshNode(OwnerA, endpoint: "tcp://127.0.0.1:5001", nodeRid: "node-a");
        var descriptorB = MeshNode(
            OwnerB,
            endpoint: "tcp://127.0.0.1:5002",
            nodeRid: "node-b",
            leaseGeneration: 2);
        var ownerAToken = Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await store.ReadOwnerLeaseAsync(OwnerA)).Token;

        await store.UpdateMeshNodeAsync(descriptorA, ZLinkLocationWriteIntent.NewClaim);
        await store.UpdateMeshNodeAsync(descriptorB, ZLinkLocationWriteIntent.NewClaim);
        await store.UpdateClientServerAsync(
            new ZLinkClientServerServerDescriptor(
                "orders",
                RoutingId.From("server-a"),
                1,
                1,
                "tcp://127.0.0.1:5101",
                100,
                ZLinkFrameworkRuntimeState.Serving,
                "test",
                OwnerA,
                ownerAToken.LeaseGeneration,
                default),
            ZLinkLocationWriteIntent.NewClaim);
        await store.UpdateFanoutPublisherAsync(
            new ZLinkFanoutPublisherDescriptor(
                "events",
                RoutingId.From("publisher-a"),
                1,
                1,
                "tcp://127.0.0.1:5201",
                ZLinkFrameworkRuntimeState.Serving,
                "test",
                OwnerA,
                ownerAToken.LeaseGeneration,
                default),
            ZLinkLocationWriteIntent.NewClaim);
        await CreateAuthorityAsync(store, OwnerA, "actor-a");
        await CreateAuthorityAsync(store, OwnerB, "actor-b");

        var removed = await store.RemoveAllByOwnerAsync(ownerAToken);

        Assert.Equal(4, removed);
        Assert.DoesNotContain((await store.ListMeshNodesAsync("play", default)).Items, row => row.OwnerId == OwnerA);
        Assert.Contains((await store.ListMeshNodesAsync("play", default)).Items, row => row.OwnerId == OwnerB);
        Assert.Empty((await store.ListClientServersAsync("orders", default)).Items);
        Assert.Empty((await store.ListFanoutPublishersAsync("events", default)).Items);
        Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(AuthorityKey("actor-a")));
        Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(AuthorityKey("actor-b")));
    }

    [Fact]
    public async Task OwnerLease_Claim_And_Renew_Use_Store_Clock()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var ttl = TimeSpan.FromSeconds(42);
        var expectedStoreNow = time.GetUtcNow();

        var claimed = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(OwnerA, ttl));
        Assert.Equal(expectedStoreNow, claimed.StoreNow);
        Assert.Equal(expectedStoreNow + ttl, claimed.LeaseExpiresAt);
        var read = Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await store.ReadOwnerLeaseAsync(OwnerA));
        Assert.Equal(claimed.Token, read.Token);
        Assert.Equal(claimed.LeaseExpiresAt, read.LeaseExpiresAt);
        time.AdvanceWallClockOnly(TimeSpan.FromMinutes(10));
        Assert.IsType<ZLinkOwnerLeaseReadResult.Missing>(
            await store.ReadOwnerLeaseAsync(OwnerA));
    }

    [Fact]
    public async Task Mesh_List_Returns_Only_The_Requested_Mesh_Snapshot()
    {
        var (store, _) = await CreateStoreWithLiveOwnersAsync(OwnerA);
        await store.UpdateMeshNodeAsync(
            MeshNode(OwnerA, nodeRid: "node-a"), ZLinkLocationWriteIntent.NewClaim);
        await store.UpdateMeshNodeAsync(
            MeshNode(OwnerA, nodeRid: "node-b", meshName: "world"),
            ZLinkLocationWriteIntent.NewClaim);

        var play = (await store.ListMeshNodesAsync("play", default)).Items;

        Assert.Equal(RoutingId.From("node-a"), Assert.Single(play).Rid);
    }

    [Fact]
    public async Task Mesh_List_Uses_Stable_Rid_Order_And_Continuation()
    {
        var (store, _) = await CreateStoreWithLiveOwnersAsync(OwnerA);
        await store.UpdateMeshNodeAsync(
            MeshNode(OwnerA, nodeRid: "node-b"),
            ZLinkLocationWriteIntent.NewClaim);
        await store.UpdateMeshNodeAsync(
            MeshNode(OwnerA, nodeRid: "node-a"),
            ZLinkLocationWriteIntent.NewClaim);

        var first = await store.ListMeshNodesAsync(
            "play",
            new ZLinkPageRequest(PageSize: 1));
        var second = await store.ListMeshNodesAsync(
            "play",
            new ZLinkPageRequest(
                PageSize: 1,
                ContinuationToken: first.ContinuationToken));

        Assert.Equal(RoutingId.From("node-a"), Assert.Single(first.Items).Rid);
        Assert.NotNull(first.ContinuationToken);
        Assert.Equal(RoutingId.From("node-b"), Assert.Single(second.Items).Rid);
        Assert.Null(second.ContinuationToken);
    }

    [Fact]
    public async Task Change_Stamp_Increments_On_Writes_And_Is_Stable_On_Reads()
    {
        var (store, _) = await CreateStoreWithLiveOwnersAsync(OwnerA);
        var before = await store.GetMeshNodeChangeStampAsync("play");
        await store.UpdateMeshNodeAsync(MeshNode(OwnerA), ZLinkLocationWriteIntent.NewClaim);
        var afterWrite = await store.GetMeshNodeChangeStampAsync("play");
        await store.ListMeshNodesAsync("play", default);
        var afterRead = await store.GetMeshNodeChangeStampAsync("play");

        Assert.True(afterWrite > before);
        Assert.Equal(afterWrite, afterRead);
    }

    private static async Task<(ZLinkInMemoryLocationStore Store, ManualTimeProvider Time)>
        CreateStoreWithLiveOwnersAsync(params string[] owners)
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        foreach (var owner in owners)
            await store.ClaimLiveOwnerAsync(owner, LeaseTtl);

        return (store, time);
    }

    private static ZLinkAuthorityKey AuthorityKey(string objectId) =>
        new($"test:actor:{objectId}");

    private static async ValueTask<ZLinkObjectReserveResult> ReserveAuthorityAsync(
        ZLinkInMemoryLocationStore store,
        string ownerId,
        string objectId)
    {
        var owner = Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await store.ReadOwnerLeaseAsync(ownerId)).Token;
        var nodeName = ownerId == OwnerB ? "node-2" : "node-1";
        var nodeRid = RoutingId.From(nodeName);
        var descriptor = MeshNode(
            ownerId,
            nodeRid: nodeName,
            leaseGeneration: owner.LeaseGeneration) with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            EntrySpotId = nodeName == "node-2"
                ? "play-entry-00000000-0000-4000-a000-000000000002"
                : "play-entry-00000000-0000-4000-a000-000000000001",
            ObjectCapabilities =
            [
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.Actor,
                    "player",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    0)
            ]
        };
        _ = await store.UpdateMeshNodeAsync(
            descriptor,
            ZLinkLocationWriteIntent.NewClaim);
        var intent = System.Text.Encoding.UTF8.GetBytes($"create:{objectId}");
        return await store.ReserveAsync(
            new ZLinkObjectReservationRequest(
                ZLinkPlacementObjectKind.Actor,
                AuthorityKey(objectId),
                "player",
                $"inline:{objectId}",
                System.Security.Cryptography.SHA256.HashData(intent),
                intent.Length,
                new ZLinkMeshNodeDescriptorKey("play", nodeRid),
                descriptor.LifecycleGeneration,
                owner,
                new byte[] { 0x11 },
                new ZLinkCapacityVector(1, 0, null)));
    }

    private static async ValueTask<ZLinkAuthoritySnapshot> CreateAuthorityAsync(
        ZLinkInMemoryLocationStore store,
        string ownerId,
        string objectId)
    {
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await ReserveAuthorityAsync(store, ownerId, objectId));
        return Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                reserved.Reservation,
                new byte[] { 0x22 })).Snapshot;
    }

    internal static ZLinkResolvedActorLocation Actor(
        string ownerId,
        string actorId = "actor-1") => new(
        "play",
        actorId,
        "player",
        new ActorRef(actorId, 1, "play", RoutingId.From("node-1")),
        OwnerNodeRid: RoutingId.From("node-1"),
        OwnerNodeGeneration: 0,
        SpotId: string.Empty,
        SpotGeneration: 0,
        SpotKind: ZLinkSpotKind.Entry,
        MembershipEpoch: 0,
        OwnerId: ownerId,
        LeaseGeneration: 1,
        UpdatedAt: default,
        AuthorityOwnerGeneration: 1);

    internal static ZLinkResolvedSpotLocation Spot(string ownerId, string spotId) => new(
        "play",
        spotId,
        SpotGeneration: 0,
        OwnerNodeRid: RoutingId.From("node-1"),
        OwnerNodeGeneration: 0,
        SpotKind: ZLinkSpotKind.User,
        SpotType: "game",
        OwnerId: ownerId,
        LeaseGeneration: 1,
        UpdatedAt: default,
        AuthorityOwnerGeneration: 1);

    internal static ZLinkMeshNodeDescriptor MeshNode(
        string ownerId,
        string endpoint = "tcp://127.0.0.1:5001",
        string nodeRid = "node-1",
        string meshName = "play",
        long leaseGeneration = 1) => new(
        meshName,
        RoutingId.From(nodeRid),
        LifecycleGeneration: 1,
        DescriptorRevision: 1,
        endpoint,
        new Dictionary<string, int>(StringComparer.Ordinal) { [meshName] = 100 },
        SecurityIdentity: string.Empty,
        OwnerId: ownerId,
        LeaseGeneration: leaseGeneration,
        UpdatedAt: default)
    {
        State = ZLinkFrameworkRuntimeState.Serving
    };
}

internal static class AuthorityLocationTestFixture
{
    internal static async ValueTask<ZLinkAuthoritySnapshot?> PublishActorAsync(
        ZLinkInMemoryLocationStore store,
        ZLinkResolvedActorLocation row,
        bool replace = false)
    {
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(row.ActorId);
        var current = await store.ReadAuthorityAsync(key);
        if (current is ZLinkAuthorityReadResult.Found found)
        {
            if (!replace) return null;
            Assert.IsType<ZLinkAuthorityCompareExchangeResult.Deleted>(
                await store.CompareExchangeAuthorityAsync(
                    key,
                    found.Snapshot.StoreVersion,
                    new ZLinkAuthorityMutation.Delete()));
        }

        var owner = await RequireOwnerAsync(store, row.OwnerId);
        var nodeGeneration = row.OwnerNodeGeneration == 0
            ? 1UL
            : row.OwnerNodeGeneration;
        await PublishDescriptorAsync(
            store,
            row.MeshName,
            row.OwnerNodeRid,
            nodeGeneration,
            owner,
            ZLinkPlacementObjectKind.Actor,
            row.ActorType);
        var payload = ZLinkActorAuthorityPayloadCodec.Encode(
            new ZLinkActorAuthorityPayload(
                ZLinkActorAuthorityState.Ready,
                row.ActorType,
                row.ActorId,
                string.IsNullOrEmpty(row.SpotId) ? "entry:test" : row.SpotId,
                row.SpotGeneration == 0 ? 1UL : row.SpotGeneration,
                row.SpotKind,
                owner.OwnerId,
                checked((ulong)owner.LeaseGeneration),
                row.MeshName,
                row.OwnerNodeRid,
                nodeGeneration));
        return await ReserveAndCommitAsync(
            store,
            key,
            ZLinkPlacementObjectKind.Actor,
            row.ActorType,
            new ZLinkMeshNodeDescriptorKey(row.MeshName, row.OwnerNodeRid),
            nodeGeneration,
            owner,
            payload,
            new ZLinkCapacityVector(1, 0, null));
    }

    internal static async ValueTask<ZLinkAuthoritySnapshot?> PublishSpotAsync(
        ZLinkInMemoryLocationStore store,
        ZLinkResolvedSpotLocation row,
        bool replace = false)
    {
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(row.SpotId);
        var current = await store.ReadAuthorityAsync(key);
        if (current is ZLinkAuthorityReadResult.Found found)
        {
            if (!replace) return null;
            Assert.IsType<ZLinkAuthorityCompareExchangeResult.Deleted>(
                await store.CompareExchangeAuthorityAsync(
                    key,
                    found.Snapshot.StoreVersion,
                    new ZLinkAuthorityMutation.Delete()));
        }

        var owner = await RequireOwnerAsync(store, row.OwnerId);
        var nodeGeneration = row.OwnerNodeGeneration == 0
            ? 1UL
            : row.OwnerNodeGeneration;
        var stableType = row.SpotType ?? "test-spot";
        await PublishDescriptorAsync(
            store,
            row.MeshName,
            row.OwnerNodeRid,
            nodeGeneration,
            owner,
            ZLinkPlacementObjectKind.UserSpot,
            stableType);
        var payload = ZLinkUserSpotAuthorityPayloadCodec.Encode(
            new ZLinkUserSpotAuthorityPayload(
                ZLinkUserSpotAuthorityState.Ready,
                stableType,
                row.SpotId,
                owner.OwnerId,
                checked((ulong)owner.LeaseGeneration),
                row.MeshName,
                row.OwnerNodeRid,
                nodeGeneration));
        return await ReserveAndCommitAsync(
            store,
            key,
            ZLinkPlacementObjectKind.UserSpot,
            stableType,
            new ZLinkMeshNodeDescriptorKey(row.MeshName, row.OwnerNodeRid),
            nodeGeneration,
            owner,
            payload,
            new ZLinkCapacityVector(
                0,
                1,
                new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.UserSpot,
                    stableType,
                    1)));
    }

    internal static ValueTask<ZLinkAuthorityReadResult> ReadActorAsync(
        ZLinkInMemoryLocationStore store,
        string actorId) =>
        store.ReadAuthorityAsync(
            ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId));

    internal static ValueTask<ZLinkAuthorityReadResult> ReadSpotAsync(
        ZLinkInMemoryLocationStore store,
        string spotId) =>
        store.ReadAuthorityAsync(
            ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId));

    private static async ValueTask<ZLinkLocationOwnerToken> RequireOwnerAsync(
        ZLinkInMemoryLocationStore store,
        string ownerId)
    {
        return Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await store.ReadOwnerLeaseAsync(ownerId)).Token;
    }

    private static async ValueTask PublishDescriptorAsync(
        ZLinkInMemoryLocationStore store,
        string meshName,
        RoutingId nodeRid,
        ulong nodeGeneration,
        ZLinkLocationOwnerToken owner,
        ZLinkPlacementObjectKind objectKind,
        string stableType)
    {
        var descriptor = new ZLinkMeshNodeDescriptor(
            meshName,
            nodeRid,
            nodeGeneration,
            1,
            "tcp://127.0.0.1:5001",
            new Dictionary<string, int>(StringComparer.Ordinal)
            {
                [meshName] = 100
            },
            string.Empty,
            owner.OwnerId,
            owner.LeaseGeneration,
            default)
        {
            State = ZLinkFrameworkRuntimeState.Serving,
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            ObjectCapabilities =
            [
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.Actor,
                    objectKind == ZLinkPlacementObjectKind.Actor
                        ? stableType
                        : "player",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    0),
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.UserSpot,
                    objectKind == ZLinkPlacementObjectKind.UserSpot
                        ? stableType
                        : "game",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    0)
            ],
            Capacity = new ZLinkPlacementCapacity(
                new ZLinkPopulationCapacity(0, 0, 0),
                new ZLinkPopulationCapacity(0, 0, 0),
                [
                    new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.UserSpot,
                        objectKind == ZLinkPlacementObjectKind.UserSpot
                            ? stableType
                            : "game",
                        0,
                        0,
                        0)
                ]),
            EntrySpotId = CreateEntrySpotId(meshName, nodeRid)
        };
        var current = await store.ListMeshNodesAsync(meshName, default);
        var existing = current.Items.SingleOrDefault(item => item.Rid == nodeRid);
        if (existing is not null
            && (existing.OwnerId != owner.OwnerId
                || existing.LeaseGeneration != owner.LeaseGeneration))
        {
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                await store.RemoveMeshNodeAsync(
                    new ZLinkMeshNodeDescriptorKey(meshName, nodeRid),
                    new ZLinkLocationOwnerToken(
                        existing.OwnerId,
                        existing.LeaseGeneration)));
            existing = null;
        }
        var intent = existing is null
            ? ZLinkLocationWriteIntent.NewClaim
            : ZLinkLocationWriteIntent.Renew;
        if (existing is not null)
            descriptor = descriptor with
            {
                DescriptorRevision = checked(existing.DescriptorRevision + 1)
            };
        var written = await store.UpdateMeshNodeAsync(descriptor, intent);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, written.Status);
    }

    private static string CreateEntrySpotId(
        string meshName,
        RoutingId nodeRid)
    {
        var hex = Convert.ToHexString(
                System.Security.Cryptography.SHA256.HashData(
                    nodeRid.ToBytes()))
            .ToLowerInvariant();
        return $"{meshName}-entry-{hex[..8]}-{hex[8..12]}"
               + $"-4{hex[13..16]}-a{hex[17..20]}-{hex[20..32]}";
    }

    private static async ValueTask<ZLinkAuthoritySnapshot> ReserveAndCommitAsync(
        ZLinkInMemoryLocationStore store,
        ZLinkAuthorityKey key,
        ZLinkPlacementObjectKind objectKind,
        string stableType,
        ZLinkMeshNodeDescriptorKey descriptor,
        ulong nodeGeneration,
        ZLinkLocationOwnerToken owner,
        byte[] payload,
        ZLinkCapacityVector capacity)
    {
        var intent = System.Text.Encoding.UTF8.GetBytes($"create:{key.Value}");
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                new ZLinkObjectReservationRequest(
                    objectKind,
                    key,
                    stableType,
                    $"inline:{key.Value}",
                    System.Security.Cryptography.SHA256.HashData(intent),
                    intent.Length,
                    descriptor,
                    nodeGeneration,
                    owner,
                    payload,
                    capacity)));
        return Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(reserved.Reservation, payload)).Snapshot;
    }
}

/// <summary>
/// Deterministic clock for lease and cache TTL tests. Wall time and the
/// monotonic timestamp advance together.
/// </summary>
internal sealed class ManualTimeProvider : TimeProvider
{
    private DateTimeOffset _utcNow = new(2026, 7, 2, 0, 0, 0, TimeSpan.Zero);
    private long _timestamp;

    public override DateTimeOffset GetUtcNow() => _utcNow;

    public override long GetTimestamp() => _timestamp;

    public override long TimestampFrequency => TimeSpan.TicksPerSecond;

    public void Advance(TimeSpan delta)
    {
        _utcNow += delta;
        _timestamp += delta.Ticks;
    }

    /// <summary>A wall-clock jump without monotonic progress, for asserting
    /// that lease expiry never compares application wall clocks (draft 6.6).</summary>
    public void AdvanceWallClockOnly(TimeSpan delta)
    {
        _utcNow += delta;
    }

    public void AdvanceMonotonicOnly(TimeSpan delta)
    {
        _timestamp += delta.Ticks;
    }
}
