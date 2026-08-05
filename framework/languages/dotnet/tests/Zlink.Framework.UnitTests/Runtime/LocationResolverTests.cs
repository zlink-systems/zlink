using System.Security.Cryptography;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.AspNetCore;

namespace Zlink.Framework.UnitTests;

public sealed class LocationResolverTests
{
    private const string OwnerA = "owner-a";
    private const string OwnerB = "owner-b";
    private static readonly TimeSpan LeaseTtl = TimeSpan.FromSeconds(15);
    private static readonly ZLinkActorLocationKey ActorKey = new("actor-1");

    [Fact]
    public async Task Positive_Ready_Route_Is_Cached_Until_Its_Captured_Max_Age()
    {
        var fixture = await FixtureAsync();
        var options = new ZLinkLocationOptions
        {
            PollingInterval = TimeSpan.Zero,
            RouteCacheMaxAge = TimeSpan.FromSeconds(10)
        };
        var observed = new ZLinkObservedLocationGenerations();
        var resolvers = new ZLinkStoreLocationResolvers(
            fixture.Store,
            new ZLinkOwnerLeaseTracker(
                fixture.Store, options, fixture.Time),
            observed,
            options: options);
        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Actor(OwnerA));

        var first = await resolvers.ResolveActorRowAsync(ActorKey);
        Assert.Equal(OwnerA, first!.OwnerId);

        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Actor(OwnerB),
            replace: true);

        var second = await resolvers.ResolveActorRowAsync(ActorKey);
        Assert.Equal(OwnerA, second!.OwnerId);

        fixture.Time.Advance(TimeSpan.FromSeconds(10));
        var afterExpiry = await resolvers.ResolveActorRowAsync(ActorKey);
        Assert.Equal(OwnerB, afterExpiry!.OwnerId);
    }

    [Fact]
    public async Task Positive_Ready_Route_Never_Outlives_The_Owner_Admission_Deadline()
    {
        var fixture = await FixtureAsync();
        var options = new ZLinkLocationOptions
        {
            PollingInterval = TimeSpan.Zero,
            RouteCacheMaxAge = TimeSpan.FromSeconds(15),
            OwnerLeaseFencingMargin = TimeSpan.FromSeconds(5)
        };
        var resolvers = new ZLinkStoreLocationResolvers(
            fixture.Store,
            new ZLinkOwnerLeaseTracker(fixture.Store, options, fixture.Time),
            new ZLinkObservedLocationGenerations(),
            options: options);
        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Actor(OwnerA));

        Assert.Equal(
            OwnerA,
            (await resolvers.ResolveActorRowAsync(ActorKey))!.OwnerId);

        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Actor(OwnerB),
            replace: true);

        fixture.Time.AdvanceMonotonicOnly(TimeSpan.FromSeconds(9));
        Assert.Equal(
            OwnerA,
            (await resolvers.ResolveActorRowAsync(ActorKey))!.OwnerId);

        fixture.Time.AdvanceMonotonicOnly(TimeSpan.FromSeconds(1));
        Assert.Equal(
            OwnerB,
            (await resolvers.ResolveActorRowAsync(ActorKey))!.OwnerId);
    }

    [Fact]
    public async Task MessageFollow_InvalidatesOnlyTheMatchingCachedRouteFence()
    {
        var fixture = await FixtureAsync();
        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Actor(OwnerA));
        var row = await fixture.Resolvers.ResolveActorRowAsync(ActorKey);
        Assert.NotNull(row);

        var source = new ZLinkServiceWireCodec.MessageFollowRoute(
            ZLinkServiceWireCodec.MessageFollowActorKind,
            row.ActorId,
            row.ActorRef.ObjectGeneration,
            row.OwnerNodeRid,
            row.OwnerNodeGeneration,
            row.AuthorityOwnerGeneration,
            checked((ulong)row.LeaseGeneration));
        var target = new ZLinkServiceWireCodec.MessageFollowRoute(
            ZLinkServiceWireCodec.MessageFollowActorKind,
            row.ActorId,
            row.ActorRef.ObjectGeneration,
            RoutingId.From("node-2"),
            row.OwnerNodeGeneration + 1,
            row.AuthorityOwnerGeneration + 1,
            checked((ulong)row.LeaseGeneration + 1));
        var record = new ZLinkServiceWireCodec.MessageFollowRecord(
            source,
            target,
            1,
            1,
            32,
            new MeshOperationId(4, 5),
            6);
        var staleSource = new ZLinkServiceWireCodec.MessageFollowRoute(
            ZLinkServiceWireCodec.MessageFollowActorKind,
            row.ActorId,
            row.ActorRef.ObjectGeneration,
            row.OwnerNodeRid,
            row.OwnerNodeGeneration,
            row.AuthorityOwnerGeneration,
            checked((ulong)row.LeaseGeneration + 1));
        var staleRecord = new ZLinkServiceWireCodec.MessageFollowRecord(
            staleSource,
            target,
            1,
            1,
            32,
            new MeshOperationId(4, 5),
            6);

        Assert.False(
            fixture.Resolvers.InvalidateMessageFollowRoute(
                staleRecord));
        Assert.True(fixture.Resolvers.HasCachedActorRoute(ActorKey));
        Assert.True(fixture.Resolvers.InvalidateMessageFollowRoute(record));
        Assert.False(fixture.Resolvers.HasCachedActorRoute(ActorKey));
    }

    [Fact]
    public async Task NotFound_Then_Claim_Is_Visible_Immediately()
    {
        var fixture = await FixtureAsync();

        Assert.Null(await fixture.Resolvers.ResolveActorRowAsync(ActorKey));

        // An actor created right after a miss must be visible to the very
        // next resolve — the create-if-absent race depends on it.
        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Actor(OwnerA));

        Assert.NotNull(await fixture.Resolvers.ResolveActorRowAsync(ActorKey));
    }

    [Fact]
    public async Task Rows_Of_Expired_Owner_Are_Not_Returned()
    {
        var fixture = await FixtureAsync();
        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Actor(OwnerA));
        Assert.NotNull(await fixture.Resolvers.ResolveActorRowAsync(ActorKey));

        // Owner A crashes: no more heartbeats, the lease runs out. The row
        // itself is never written again, yet resolve must stop returning it.
        fixture.Time.Advance(LeaseTtl + TimeSpan.FromSeconds(1));

        Assert.Null(await fixture.Resolvers.ResolveActorRowAsync(ActorKey));

        var resolution = await fixture.Resolvers.ResolveActorRowWithStatusAsync(
            ActorKey);
        Assert.Null(resolution.Row);
        Assert.Equal(
            ZLinkLocationResolutionKind.KnownUnavailable,
            resolution.Kind);
    }

    [Fact]
    public async Task Missing_Actor_Is_Distinguished_From_An_Expired_Owner()
    {
        var fixture = await FixtureAsync();

        var resolution = await fixture.Resolvers.ResolveActorRowWithStatusAsync(
            ActorKey);

        Assert.Null(resolution.Row);
        Assert.Equal(
            ZLinkLocationResolutionKind.Missing,
            resolution.Kind);
    }

    [Fact]
    public async Task Expired_Actor_Authority_Requires_Recovery_Before_Recreation()
    {
        var fixture = await FixtureAsync();
        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Actor(OwnerA) with
            {
                ActorRef = new ActorRef("actor-1", 10, "play", RoutingId.From("node-1")),
                MembershipEpoch = 2
            });
        Assert.NotNull(await fixture.Resolvers.ResolveActorRowAsync(ActorKey));

        // Expiry hides the actor from resolution but does not delete authority.
        // A replacement owner cannot bypass the recovery protocol by claiming
        // the same key as a new object.
        fixture.Time.Advance(LeaseTtl + TimeSpan.FromSeconds(1));
        Assert.Null(await fixture.Resolvers.ResolveActorRowAsync(ActorKey));

        var replacement = InMemoryLocationStoreTests.Actor(OwnerB) with
        {
            ActorRef = new ActorRef("actor-1", 20, "play", RoutingId.From("node-2")),
            OwnerNodeRid = RoutingId.From("node-2"),
            MembershipEpoch = 0
        };
        var stale = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await AuthorityLocationTestFixture.ReadActorAsync(
                fixture.Store,
                "actor-1"));
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Conflict>(
            await fixture.Store.CompareExchangeAuthorityAsync(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey("actor-1"),
                stale.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Delete()));
        Assert.Null(await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            replacement));

        Assert.Null(await fixture.Resolvers.ResolveActorRowAsync(ActorKey));
    }

    [Fact]
    public async Task Expired_Spot_Authority_Requires_Recovery_Before_Recreation()
    {
        var fixture = await FixtureAsync();
        const string spotId = "spot-recreated";
        var key = new ZLinkSpotLocationKey(spotId);
        await AuthorityLocationTestFixture.PublishSpotAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Spot(OwnerA, "spot-recreated") with
            {
                SpotGeneration = 10
            });
        Assert.NotNull(await fixture.Resolvers.ResolveSpotRowAsync(key));

        fixture.Time.Advance(LeaseTtl + TimeSpan.FromSeconds(1));
        Assert.Null(await fixture.Resolvers.ResolveSpotRowAsync(key));

        var resolution = await fixture.Resolvers.ResolveSpotRowWithStatusAsync(key);
        Assert.Null(resolution.Row);
        Assert.Equal(
            ZLinkLocationResolutionKind.KnownUnavailable,
            resolution.Kind);

        var claim = await AuthorityLocationTestFixture.PublishSpotAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Spot(OwnerB, "spot-recreated") with
            {
                OwnerNodeRid = RoutingId.From("node-2"),
                SpotGeneration = 1
            });
        Assert.Null(claim);

        Assert.Null(await fixture.Resolvers.ResolveSpotRowAsync(key));
    }

    [Fact]
    public async Task Actor_Row_Is_Not_Returned_Before_The_Reference_Generation_Is_Published()
    {
        var fixture = await FixtureAsync();
        await ReserveCreatingActorAsync(fixture.Store);

        Assert.Null(await fixture.Resolvers.ResolveActorRowAsync(ActorKey));
    }

    [Fact]
    public async Task MeshNode_List_Excludes_Expired_Owners()
    {
        var fixture = await FixtureAsync();
        await fixture.Store.UpdateMeshNodeAsync(
            InMemoryLocationStoreTests.MeshNode(OwnerA), ZLinkLocationWriteIntent.NewClaim);
        await fixture.Store.UpdateMeshNodeAsync(
            InMemoryLocationStoreTests.MeshNode(
                OwnerB,
                "tcp://127.0.0.1:5002",
                "node-2",
                leaseGeneration: fixture.OwnerB.LeaseGeneration),
            ZLinkLocationWriteIntent.NewClaim);

        var both = await fixture.Resolvers.ListLiveMeshNodesAsync("play");
        Assert.Equal(2, both.Count);

        // Owner A's lease expires; the descriptor drops out of the desired
        // set within one polling interval without any row write.
        fixture.Time.Advance(LeaseTtl + TimeSpan.FromSeconds(1));

        var survivors = await fixture.Resolvers.ListLiveMeshNodesAsync("play");
        Assert.Single(survivors);
        Assert.Equal(OwnerB, survivors[0].OwnerId);
    }

    [Fact]
    public async Task MeshNode_Successor_Owner_Can_Restart_Generation_Without_An_Observed_Miss()
    {
        var fixture = await FixtureAsync();
        var predecessor = InMemoryLocationStoreTests.MeshNode(OwnerA) with
        {
            LifecycleGeneration = 20,
            DescriptorRevision = 4
        };
        await fixture.Store.UpdateMeshNodeAsync(
            predecessor, ZLinkLocationWriteIntent.NewClaim);
        Assert.Equal(
            20UL,
            Assert.Single(await fixture.Resolvers.ListLiveMeshNodesAsync("play"))
                .LifecycleGeneration);

        // The store takeover replaces the row atomically. A polling reader can
        // therefore see the new owner without ever observing a missing key.
        // Core generations are process-local wall-clock values, so the new
        // owner may legitimately start below the predecessor's value.
        fixture.Time.Advance(LeaseTtl + TimeSpan.FromSeconds(1));
        var successorOwner = fixture.OwnerB;
        var successor = InMemoryLocationStoreTests.MeshNode(
            OwnerB,
            "tcp://127.0.0.1:5002",
            "node-1",
            leaseGeneration: successorOwner.LeaseGeneration) with
        {
            LifecycleGeneration = 10,
            DescriptorRevision = 1
        };
        var takeover = await fixture.Store.UpdateMeshNodeAsync(
            successor, ZLinkLocationWriteIntent.Takeover);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, takeover.Status);

        var resolved = Assert.Single(await fixture.Resolvers.ListLiveMeshNodesAsync("play"));
        Assert.Equal(OwnerB, resolved.OwnerId);
        Assert.Equal(10UL, resolved.LifecycleGeneration);

        // Once the successor is accepted, a delayed predecessor row must not
        // move the observed incarnation back to its retired owner.
        var observed = new ZLinkObservedLocationGenerations();
        Assert.True(observed.AcceptDescriptor(predecessor));
        Assert.True(observed.AcceptDescriptor(successor));
        Assert.False(observed.AcceptDescriptor(predecessor));
    }

    [Fact]
    public async Task MeshNode_List_Rereads_When_A_Local_Revision_Update_Races_The_Snapshot()
    {
        var time = new ManualTimeProvider();
        var inner = new ZLinkInMemoryLocationStore(time);
        var owner = await inner.ClaimLiveOwnerAsync(OwnerA, LeaseTtl);
        var initial = InMemoryLocationStoreTests.MeshNode(
            OwnerA,
            leaseGeneration: owner.LeaseGeneration);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await inner.UpdateMeshNodeAsync(
                initial,
                ZLinkLocationWriteIntent.NewClaim)).Status);
        var current = initial with { DescriptorRevision = 2 };
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await inner.UpdateMeshNodeAsync(
                current,
                ZLinkLocationWriteIntent.Renew)).Status);

        var store = new StaleFirstMeshNodeListStore(inner, initial);
        var options = new ZLinkLocationOptions
        {
            PollingInterval = TimeSpan.Zero
        };
        var observed = new ZLinkObservedLocationGenerations();
        observed.ObserveDescriptor(current);
        var resolvers = new ZLinkStoreLocationResolvers(
            store,
            new ZLinkOwnerLeaseTracker(store, options, time),
            observed,
            options: options,
            timeProvider: time);

        var live = await resolvers.ListLiveMeshNodesAsync("play");

        var row = Assert.Single(live);
        Assert.Equal(2UL, row.DescriptorRevision);
        Assert.Equal(2, store.ListCalls);
    }

    [Fact]
    public async Task Spot_Address_Uses_Global_Id_And_Returns_The_Canonical_Row_Mesh()
    {
        var fixture = await FixtureAsync();
        await AuthorityLocationTestFixture.PublishSpotAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Spot(OwnerA, "spot-1"));

        var addresses = new ZLinkLocationAddressResolvers(
            fixture.Resolvers,
            new ZLinkSpotHandleRegistry());

        var address = Assert.IsType<ZLinkResolvedSpotHandle>(
            await addresses.ResolveSpotHandleAsync("spot-1"));

        Assert.Equal("spot-1", address.SpotId);
        Assert.Equal(RoutingId.From("node-1"), address.Snapshot.NodeRid);
        Assert.Equal(ZLinkSpotKind.User, address.Snapshot.SpotKind);

        var sameGlobalAddress = Assert.IsType<ZLinkResolvedSpotHandle>(
            await addresses.ResolveSpotHandleAsync("spot-1"));
        Assert.Equal("play", sameGlobalAddress.MeshName);
    }

    [Fact]
    public async Task Spot_Address_Uses_The_Canonical_Row_Mesh_For_A_Global_Id()
    {
        var fixture = await FixtureAsync();
        const string sharedRid = "shared-entry";
        await AuthorityLocationTestFixture.PublishSpotAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Spot(OwnerA, "shared-entry") with
            {
                MeshName = "external",
                SpotId = sharedRid,
                OwnerNodeRid = RoutingId.From("node-1")
            });
        var addresses = new ZLinkLocationAddressResolvers(
            fixture.Resolvers,
            new ZLinkSpotHandleRegistry());

        var handle = Assert.IsType<ZLinkResolvedSpotHandle>(
            await addresses.ResolveSpotHandleAsync(sharedRid));

        Assert.Equal("external", handle.MeshName);
        Assert.Equal(RoutingId.From("node-1"), handle.Snapshot.NodeRid);
    }

    [Fact]
    public async Task Actor_Handle_Internal_Snapshot_Preserves_Entry_Owner_And_Kind()
    {
        var fixture = await FixtureAsync();
        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Actor(OwnerA, "actor-entry"));
        var addresses = new ZLinkLocationAddressResolvers(
            fixture.Resolvers,
            new ZLinkSpotHandleRegistry());

        var handle = Assert.IsType<ZLinkResolvedSpotHandle>(
            await addresses.ResolveActorSpotHandleAsync("actor-entry"));

        Assert.Equal(RoutingId.From("node-1"), handle.Snapshot.NodeRid);
        Assert.Equal(handle.Snapshot.SpotId, handle.SpotId);
        Assert.Equal(ZLinkSpotKind.Entry, handle.Snapshot.SpotKind);
        Assert.Null(typeof(IZLinkLocationRepository).Assembly.GetType(
            "Zlink.Framework.Contracts.Locations.SpotHandle"));
    }

    [Fact]
    public async Task Actor_Handles_With_The_Same_Id_Are_Global_Across_MeshNames()
    {
        var fixture = await FixtureAsync();
        var play = InMemoryLocationStoreTests.Actor(OwnerA, "shared-actor") with
        {
            SpotKind = ZLinkSpotKind.User,
            SpotId = "play-spot"
        };
        var external = InMemoryLocationStoreTests.Actor(OwnerB, "shared-actor") with
        {
            MeshName = "external",
            ActorRef = new ActorRef("shared-actor", 1, "play", RoutingId.From("node-2")),
            OwnerNodeRid = RoutingId.From("node-2"),
            SpotKind = ZLinkSpotKind.User,
            SpotId = "external-spot"
        };
        Assert.NotNull(await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            play));
        Assert.Null(await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            external));
        var handles = new ZLinkSpotHandleRegistry();
        var addresses = new ZLinkLocationAddressResolvers(fixture.Resolvers, handles);

        var playHandle = Assert.IsType<ZLinkResolvedSpotHandle>(
            await addresses.ResolveActorSpotHandleAsync("shared-actor"));
        var externalHandle = Assert.IsType<ZLinkResolvedSpotHandle>(
            await addresses.ResolveActorSpotHandleAsync("shared-actor"));

        handles.UpdateActor(external with
        {
            SpotId = "external-moved",
            MembershipEpoch = external.MembershipEpoch + 1
        });

        Assert.Equal(playHandle.MeshName, externalHandle.MeshName);
        Assert.Equal("external-moved", playHandle.SpotId);
        Assert.Equal("external-moved", externalHandle.SpotId);
    }

    [Fact]
    public async Task Spot_And_Actor_Handles_Preserve_The_Row_MeshName_Across_Refresh()
    {
        var fixture = await FixtureAsync();
        await AuthorityLocationTestFixture.PublishSpotAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Spot(OwnerA, "spot-mapped"));
        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            InMemoryLocationStoreTests.Actor(OwnerA, "actor-mapped") with
            {
                SpotKind = ZLinkSpotKind.User,
                SpotId = "spot-mapped"
            });

        var handles = new ZLinkSpotHandleRegistry();
        var addresses = new ZLinkLocationAddressResolvers(
            fixture.Resolvers,
            handles);

        var spot = Assert.IsType<ZLinkResolvedSpotHandle>(
            await addresses.ResolveSpotHandleAsync("spot-mapped"));
        var actor = Assert.IsType<ZLinkResolvedSpotHandle>(
            await addresses.ResolveActorSpotHandleAsync("actor-mapped"));
        Assert.Equal("play", spot.Snapshot.RouterChannelId);
        Assert.Equal("play", actor.Snapshot.RouterChannelId);

        handles.UpdateSpot(InMemoryLocationStoreTests.Spot(OwnerA, "spot-mapped") with
        {
            SpotGeneration = 2
        });
        handles.UpdateActor(InMemoryLocationStoreTests.Actor(OwnerA, "actor-mapped") with
        {
            SpotKind = ZLinkSpotKind.User,
            SpotId = "spot-mapped",
            MembershipEpoch = 2
        });
        Assert.Equal("play", spot.Snapshot.RouterChannelId);
        Assert.Equal("play", actor.Snapshot.RouterChannelId);
    }

    [Fact]
    public async Task Spot_Handle_Request_Does_Not_Resubmit_After_Target_Not_Found()
    {
        var fixture = await FixtureAsync();
        var initial = InMemoryLocationStoreTests.Spot(OwnerA, "spot-refresh");
        await AuthorityLocationTestFixture.PublishSpotAsync(
            fixture.Store,
            initial);
        var addresses = new ZLinkLocationAddressResolvers(
            fixture.Resolvers,
            new ZLinkSpotHandleRegistry());
        var handle = Assert.IsType<ZLinkResolvedSpotHandle>(
            await addresses.ResolveSpotHandleAsync(initial.SpotId));

        await AuthorityLocationTestFixture.PublishSpotAsync(
            fixture.Store,
            initial with { OwnerNodeRid = RoutingId.From("node-2"), OwnerId = OwnerB },
            replace: true);
        var attempts = 0;
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await ZLinkSpotHandleRequestExecution.ExecuteAsync<bool>(
                handle,
                _ =>
                {
                    attempts++;
                    return ValueTask.FromException<bool>(
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.NotFound,
                            "stale route"));
                },
                CancellationToken.None));

        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
        Assert.Equal(ZLinkRetryAdvice.DoNotRetry, error.RetryAdvice);
        Assert.Equal(1, attempts);
    }

    [Fact]
    public async Task Spot_Handle_Unavailable_Route_Is_Invalidated_Without_Resubmit()
    {
        var invalidated = false;
        var handle = new ZLinkResolvedSpotHandle(
            new ZLinkSpotHandleSnapshot(
                "play",
                RoutingId.From("node-1"),
                "spot-unavailable",
                1),
            1,
            _ => ValueTask.FromResult<
                (ZLinkSpotHandleSnapshot Snapshot, ulong Version)?>(null),
            () => invalidated = true);

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await ZLinkSpotHandleRequestExecution.ExecuteAsync<bool>(
                handle,
                _ => ValueTask.FromException<bool>(
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        "stale route",
                        ZLinkRetryAdvice.RetryAfterBackoff)),
                CancellationToken.None));

        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
        Assert.True(invalidated);
    }

    [Fact]
    public void Spot_Handle_Registry_Uses_Global_SpotId_Across_Mesh_Labels()
    {
        const string spotId = "shared-spot";
        var handle = new ZLinkResolvedSpotHandle(
            new ZLinkSpotHandleSnapshot("play", RoutingId.From("node-1"), spotId, 1),
            1,
            _ => ValueTask.FromResult<(ZLinkSpotHandleSnapshot, ulong)?>(null));
        var handles = new ZLinkSpotHandleRegistry();
        handles.RegisterSpot(new ZLinkSpotLocationKey(spotId), handle);

        handles.UpdateSpot(InMemoryLocationStoreTests.Spot(OwnerB, "shared-spot") with
        {
            MeshName = "other",
            OwnerNodeRid = RoutingId.From("node-2"),
            SpotGeneration = 2
        });
        handles.RemoveSpot(new ZLinkSpotLocationKey(spotId), 3);

        Assert.Throws<ZLinkFrameworkException>(() => _ = handle.Snapshot);
    }

    [Fact]
    public void Polling_Refresh_Invalidates_Handles_Whose_Row_Vanished()
    {
        const string spotId = "shared-spot";
        var key = new ZLinkSpotLocationKey(spotId);
        var first = new ZLinkResolvedSpotHandle(
            new ZLinkSpotHandleSnapshot("play", RoutingId.From("node-1"), spotId, 1),
            1,
            _ => ValueTask.FromResult<(ZLinkSpotHandleSnapshot, ulong)?>(null));
        var second = new ZLinkResolvedSpotHandle(
            new ZLinkSpotHandleSnapshot("play", RoutingId.From("node-2"), spotId, 2),
            2,
            _ => ValueTask.FromResult<(ZLinkSpotHandleSnapshot, ulong)?>(null));
        var handles = new ZLinkSpotHandleRegistry();
        handles.RegisterSpot(key, first);
        handles.RegisterSpot(key, second);

        foreach (var handle in handles.SnapshotLiveHandles())
            handle.InvalidateCurrent();

        Assert.Throws<ZLinkFrameworkException>(() => _ = first.Snapshot);
        Assert.Throws<ZLinkFrameworkException>(() => _ = second.Snapshot);
    }

    [Fact]
    public async Task Watch_Upsert_Applies_The_Current_Row_And_Preserves_The_MeshName()
    {
        var fixture = await FixtureAsync();
        var initial = InMemoryLocationStoreTests.Spot(OwnerA, "spot-watch-map");
        await AuthorityLocationTestFixture.PublishSpotAsync(
            fixture.Store,
            initial);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.FromMinutes(1) };
        var handles = new ZLinkSpotHandleRegistry();
        var resolver = new ZLinkLocationAddressResolvers(
            fixture.Resolvers,
            handles);
        var handle = Assert.IsType<ZLinkResolvedSpotHandle>(
            await resolver.ResolveSpotHandleAsync(initial.SpotId));
        var takeover = Assert.IsType<ZLinkAuthoritySnapshot>(
            await AuthorityLocationTestFixture.PublishSpotAsync(
            fixture.Store,
            initial with { OwnerId = OwnerB, OwnerNodeRid = RoutingId.From("node-2") },
            replace: true));
        await using var host = new ZLinkSpotHandleWatchHost(
            null,
            fixture.Resolvers,
            handles,
            options);

        await host.ApplyAsync(
            new ZLinkLocationChanged(
                ZLinkLocationKind.Spot,
                new ZLinkLocationKey.Spot(new ZLinkSpotLocationKey(initial.SpotId)),
                ZLinkLocationChangeType.Upserted,
                takeover.AuthorityOwnerGeneration,
                DateTimeOffset.UtcNow),
            CancellationToken.None);

        Assert.Equal(RoutingId.From("node-2"), handle.Snapshot.NodeRid);
        Assert.Equal("play", handle.Snapshot.RouterChannelId);
    }

    [Fact]
    public async Task Watch_Remove_Invalidates_The_Handle_Until_A_Newer_Row_Appears()
    {
        var fixture = await FixtureAsync();
        var initial = InMemoryLocationStoreTests.Spot(OwnerA, "spot-watch");
        var written = Assert.IsType<ZLinkAuthoritySnapshot>(
            await AuthorityLocationTestFixture.PublishSpotAsync(
                fixture.Store,
                initial));
        var handles = new ZLinkSpotHandleRegistry();
        var resolver = new ZLinkLocationAddressResolvers(
            fixture.Resolvers,
            handles);
        var handle = Assert.IsType<ZLinkResolvedSpotHandle>(
            await resolver.ResolveSpotHandleAsync(initial.SpotId));
        await using var host = new ZLinkSpotHandleWatchHost(
            null,
            fixture.Resolvers,
            handles,
            new ZLinkLocationOptions { PollingInterval = TimeSpan.FromMinutes(1) });

        await host.ApplyAsync(
            new ZLinkLocationChanged(
                ZLinkLocationKind.Spot,
                new ZLinkLocationKey.Spot(new ZLinkSpotLocationKey(initial.SpotId)),
                ZLinkLocationChangeType.Removed,
                written.AuthorityOwnerGeneration,
                DateTimeOffset.UtcNow),
            CancellationToken.None);

        Assert.Throws<ZLinkFrameworkException>(() => _ = handle.Snapshot);

        handles.UpdateSpot(initial with
        {
            OwnerNodeRid = RoutingId.From("node-recovered"),
            SpotGeneration = written.ObjectGeneration + 1
        });

        Assert.Equal(RoutingId.From("node-recovered"), handle.Snapshot.NodeRid);
    }

    [Fact]
    public async Task Spot_Handle_Request_Does_Not_Refresh_On_Route_Not_Connected()
    {
        var refreshCalls = 0;
        var operationCalls = 0;
        var handle = new ZLinkResolvedSpotHandle(
            new ZLinkSpotHandleSnapshot(
                "play",
                RoutingId.From("node-1"),
                "spot-1",
                1),
            1,
            _ =>
            {
                refreshCalls++;
                return ValueTask.FromResult<(ZLinkSpotHandleSnapshot, ulong)?>(null);
            });

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await ZLinkSpotHandleRequestExecution.ExecuteAsync<bool>(
                handle,
                _ =>
                {
                    operationCalls++;
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        "route is converging");
                },
                CancellationToken.None));

        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
        Assert.Equal(1, operationCalls);
        Assert.Equal(0, refreshCalls);
    }

    [Fact]
    public void Spot_Handle_Does_Not_Apply_An_Older_Version()
    {
        var handle = new ZLinkResolvedSpotHandle(
            new ZLinkSpotHandleSnapshot(
                "play",
                RoutingId.From("node-new"),
                "spot-new",
                2),
            2,
            _ => ValueTask.FromResult<(ZLinkSpotHandleSnapshot, ulong)?>(null));

        handle.Update(
            new ZLinkSpotHandleSnapshot(
                "play",
                RoutingId.From("node-old"),
                "spot-old",
                1),
            1);
        handle.Invalidate(1);

        Assert.Equal(RoutingId.From("node-new"), handle.Snapshot.NodeRid);
        Assert.Equal("spot-new", handle.SpotId);

        handle.Invalidate(3);
        handle.Update(
            new ZLinkSpotHandleSnapshot(
                "play",
                RoutingId.From("node-same"),
                "spot-same",
                3),
            3);
        handle.Update(
            new ZLinkSpotHandleSnapshot(
                "play",
                RoutingId.From("node-delayed"),
                "spot-delayed",
                2),
            2);
        Assert.Throws<ZLinkFrameworkException>(() => _ = handle.Snapshot);
    }

    [Fact]
    public async Task Actor_Address_Is_The_Entry_Spot_For_Entry_Actors_And_The_User_Spot_Otherwise()
    {
        var fixture = await FixtureAsync();
        var addresses = new ZLinkLocationAddressResolvers(
            fixture.Resolvers,
            new ZLinkSpotHandleRegistry());

        var entryActor = InMemoryLocationStoreTests.Actor(OwnerA) with
        {
            SpotId = "entry:test",
            SpotGeneration = 1
        };
        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            entryActor);

        var entryAddress = await addresses.ResolveActorSpotHandleAsync(entryActor.ActorId);
        Assert.NotNull(entryAddress);
        Assert.Equal(entryActor.SpotId, entryAddress.SpotId);

        var userActor = InMemoryLocationStoreTests.Actor(OwnerA, "actor-2") with
        {
            ActorRef = new ActorRef("actor-2", 1, "play", RoutingId.From("node-1")),
            SpotKind = ZLinkSpotKind.User,
            SpotId = "spot-7"
        };
        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            userActor);

        var userAddress = await addresses.ResolveActorSpotHandleAsync(userActor.ActorId);
        Assert.NotNull(userAddress);
        Assert.Equal("spot-7", userAddress.SpotId);
    }

    [Fact]
    public async Task Actor_Spot_Handle_Does_Not_Resubmit_After_The_Actor_Moves()
    {
        var fixture = await FixtureAsync();
        var actor = InMemoryLocationStoreTests.Actor(OwnerA) with
        {
            SpotKind = ZLinkSpotKind.User,
            SpotId = "spot-old"
        };
        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            actor);
        var addresses = new ZLinkLocationAddressResolvers(
            fixture.Resolvers,
            new ZLinkSpotHandleRegistry());
        var handle = Assert.IsType<ZLinkResolvedSpotHandle>(
            await addresses.ResolveActorSpotHandleAsync(actor.ActorId));

        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            actor with
            {
                OwnerId = OwnerB,
                OwnerNodeRid = RoutingId.From("node-2"),
                SpotId = "spot-new",
                MembershipEpoch = actor.MembershipEpoch + 1
            },
            replace: true);

        var attempts = 0;
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await ZLinkSpotHandleRequestExecution.ExecuteAsync<bool>(
                handle,
                _ =>
                {
                    attempts++;
                    return ValueTask.FromException<bool>(
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.NotFound,
                            "actor moved"));
                },
                CancellationToken.None));

        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
        Assert.Equal(1, attempts);
        Assert.Equal("spot-old", handle.SpotId);
    }

    [Fact]
    public async Task Location_Event_Updates_Existing_Actor_Spot_Handle_Snapshot()
    {
        var fixture = await FixtureAsync();
        var actor = InMemoryLocationStoreTests.Actor(OwnerA) with
        {
            SpotKind = ZLinkSpotKind.User,
            SpotId = "spot-old"
        };
        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            actor);
        var handles = new ZLinkSpotHandleRegistry();
        var addresses = new ZLinkLocationAddressResolvers(
            fixture.Resolvers,
            handles);
        var handle = Assert.IsType<ZLinkResolvedSpotHandle>(
            await addresses.ResolveActorSpotHandleAsync(actor.ActorId));

        handles.UpdateActor(actor with
        {
            OwnerNodeRid = RoutingId.From("node-2"),
            SpotId = "spot-new",
            MembershipEpoch = actor.MembershipEpoch + 1
        });

        Assert.Equal(RoutingId.From("node-2"), handle.Snapshot.NodeRid);
        Assert.Equal("spot-new", handle.SpotId);
    }

    [Fact]
    public async Task Handle_Polling_Updates_Actor_Snapshot_When_Watch_Is_Unavailable()
    {
        var fixture = await FixtureAsync();
        var actor = InMemoryLocationStoreTests.Actor(OwnerA) with
        {
            SpotKind = ZLinkSpotKind.User,
            SpotId = "spot-old"
        };
        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            actor);
        var locationOptions = new ZLinkLocationOptions
        {
            PollingInterval = TimeSpan.FromMilliseconds(10)
        };
        var handles = new ZLinkSpotHandleRegistry();
        var addresses = new ZLinkLocationAddressResolvers(
            fixture.Resolvers,
            handles);
        var handle = Assert.IsType<ZLinkResolvedSpotHandle>(
            await addresses.ResolveActorSpotHandleAsync(actor.ActorId));
        await using var host = new ZLinkSpotHandleWatchHost(
            null,
            fixture.Resolvers,
            handles,
            locationOptions);
        await host.StartAsync(CancellationToken.None);

        await AuthorityLocationTestFixture.PublishActorAsync(
            fixture.Store,
            actor with
            {
                OwnerId = OwnerB,
                OwnerNodeRid = RoutingId.From("node-2"),
                SpotId = "spot-new",
                MembershipEpoch = actor.MembershipEpoch + 1
            },
            replace: true);

        fixture.Time.Advance(TimeSpan.FromSeconds(15));
        await WaitUntilAsync(
            () => handle.SpotId == "spot-new",
            TimeSpan.FromSeconds(2));
        Assert.Equal(RoutingId.From("node-2"), handle.Snapshot.NodeRid);
        Assert.Equal("play", handle.Snapshot.RouterChannelId);
    }

    [Fact]
    public async Task Spot_Handle_Watch_Host_Disposal_Is_Idempotent()
    {
        var fixture = await FixtureAsync();
        var host = new ZLinkSpotHandleWatchHost(
            null,
            fixture.Resolvers,
            new ZLinkSpotHandleRegistry(),
            new ZLinkLocationOptions { PollingInterval = TimeSpan.FromMilliseconds(10) });
        await host.StartAsync(CancellationToken.None);
        await Task.WhenAll(
            host.StopAsync(CancellationToken.None),
            host.DisposeAsync().AsTask(),
            host.DisposeAsync().AsTask());
        await host.DisposeAsync();
    }

    private static async ValueTask ReserveCreatingActorAsync(
        ZLinkInMemoryLocationStore store)
    {
        var actor = InMemoryLocationStoreTests.Actor(OwnerA) with
        {
            OwnerNodeGeneration = 1,
            SpotId = "entry-node-1",
            SpotGeneration = 1
        };
        var committed = Assert.IsType<ZLinkAuthoritySnapshot>(
            await AuthorityLocationTestFixture.PublishActorAsync(store, actor));
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.ActorId);
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Deleted>(
            await store.CompareExchangeAuthorityAsync(
                key,
                committed.StoreVersion,
                new ZLinkAuthorityMutation.Delete()));
        var owner = Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await store.ReadOwnerLeaseAsync(OwnerA)).Token;
        var creating = ZLinkActorAuthorityPayloadCodec.Encode(
            new ZLinkActorAuthorityPayload(
                ZLinkActorAuthorityState.Creating,
                actor.ActorType,
                actor.ActorId,
                actor.SpotId,
                actor.SpotGeneration,
                actor.SpotKind,
                owner.OwnerId,
                checked((ulong)owner.LeaseGeneration),
                actor.MeshName,
                actor.OwnerNodeRid,
                actor.OwnerNodeGeneration));
        var intent = System.Text.Encoding.UTF8.GetBytes("create:actor-1");
        Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                new ZLinkObjectReservationRequest(
                    ZLinkPlacementObjectKind.Actor,
                    key,
                    actor.ActorType,
                    "inline:actor-1",
                    SHA256.HashData(intent),
                    intent.Length,
                    new ZLinkMeshNodeDescriptorKey(actor.MeshName, actor.OwnerNodeRid),
                    actor.OwnerNodeGeneration,
                    owner,
                    creating,
                    new ZLinkCapacityVector(1, 0, null))));
    }

    private static async Task WaitUntilAsync(Func<bool> condition, TimeSpan timeout)
    {
        using var cancellation = new CancellationTokenSource(timeout);
        while (!condition())
            await Task.Delay(10, cancellation.Token);
    }

    [Fact]
    public async Task Older_Membership_Epoch_From_A_Lagging_Replica_Is_Never_A_Success()
    {
        await Task.Yield();
        var observed = new ZLinkObservedLocationGenerations();
        var epoch2 = InMemoryLocationStoreTests.Actor(OwnerA) with
        {
            MembershipEpoch = 2
        };
        var epoch1 = epoch2 with { MembershipEpoch = 1 };

        Assert.True(observed.AcceptActor(epoch2));
        Assert.False(observed.AcceptActor(epoch1));
        Assert.True(observed.AcceptActor(epoch2));
    }

    private static ZLinkFrameworkRegistration PlayRegistration()
    {
        var registration = new ZLinkFrameworkRegistration();
        registration.SpotNodes.Add(
            "play",
            new ZLinkSpotNodeRegistration
            {
                SpotNodeName = "play",
                SpotMeshChannelName = "play"
            });
        return registration;
    }

    private static async Task<ResolverFixture> FixtureAsync()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var ownerA = await store.ClaimLiveOwnerAsync(OwnerA, LeaseTtl);
        var ownerB = await store.ClaimLiveOwnerAsync(
            OwnerB,
            TimeSpan.FromMinutes(5));

        var options = new ZLinkLocationOptions
        {
            // Keep the lease snapshot maximally fresh in unit tests so lease
            // expiry is observed on the next read.
            PollingInterval = TimeSpan.Zero
        };

        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var observed = new ZLinkObservedLocationGenerations();
        var resolvers = new ZLinkStoreLocationResolvers(
            store, tracker, observed);
        return new ResolverFixture(store, resolvers, time, ownerA, ownerB);
    }

    private sealed record ResolverFixture(
        ZLinkInMemoryLocationStore Store,
        ZLinkStoreLocationResolvers Resolvers,
        ManualTimeProvider Time,
        ZLinkLocationOwnerToken OwnerA,
        ZLinkLocationOwnerToken OwnerB);

    private sealed class StaleFirstMeshNodeListStore(
        ZLinkInMemoryLocationStore inner,
        ZLinkMeshNodeDescriptor staleRow) : ZLinkLocationStoreTestDouble
    {
        internal int ListCalls { get; private set; }

        public override ValueTask<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>
            ListMeshNodesAsync(
                string meshName,
                ZLinkPageRequest page,
                CancellationToken cancellationToken = default)
        {
            ListCalls++;
            if (ListCalls == 1)
            {
                return ValueTask.FromResult(
                    new ZLinkLocationPage<ZLinkMeshNodeDescriptor>(
                        [staleRow],
                        null));
            }

            return inner.ListMeshNodesAsync(meshName, page, cancellationToken);
        }

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default) =>
            inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);
    }
}
