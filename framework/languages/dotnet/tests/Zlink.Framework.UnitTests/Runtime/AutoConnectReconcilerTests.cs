using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class AutoConnectReconcilerTests
{
    private static readonly TimeSpan LeaseTtl = TimeSpan.FromSeconds(15);

    [Fact]
    public void Planner_Excludes_Self_And_Foreign_Meshes()
    {
        var local = Local(ZLinkLocationAutoConnectType.ClientServer, ZLinkLocationRole.Dealer, "local", "tcp://l:1");
        var descriptors = new[]
        {
            Descriptor("r1", "tcp://r:1"),
            // Same rid as local: excluded as self.
            Descriptor("local", "tcp://r:2"),
            // Different mesh: ignored.
            Descriptor("r2", "tcp://r:3", mesh: "other")
        };

        var desired = ZLinkAutoConnectPlanner.ComputeDesired(local, descriptors);

        var target = Assert.Single(desired.Values);
        Assert.Equal("tcp://r:1", target.Endpoint);
        Assert.Equal(1, ZLinkAutoConnectPlanner.CountDiscoveredPeers(local, descriptors));
    }

    [Fact]
    public void Planner_KeepsDistinctPeersThatRequestedPortZero()
    {
        var local = Local(
            ZLinkLocationAutoConnectType.RouteMesh,
            ZLinkLocationRole.Router,
            "aa",
            "tcp://127.0.0.1:31001");
        var remote = Descriptor("bb", "tcp://127.0.0.1:31002");

        var target = Assert.Single(
            ZLinkAutoConnectPlanner.ComputeDesired(local, [remote])).Value;

        Assert.Equal("tcp://127.0.0.1:31002", target.Endpoint);
    }

    [Fact]
    public void Planner_Marks_Draining_Descriptors_Instead_Of_Dropping_Them()
    {
        var local = Local(
            ZLinkLocationAutoConnectType.ClientServer,
            ZLinkLocationRole.Dealer,
            "local",
            "tcp://l:1");
        var draining = Descriptor("remote", "tcp://r:1") with
        {
            State = ZLinkFrameworkRuntimeState.Draining
        };

        var target = Assert.Single(ZLinkAutoConnectPlanner.ComputeDesired(local, [draining])).Value;

        // A draining node stays in the desired set so an already-active
        // connection is not cut; the reconciler skips it for new dials.
        Assert.Equal("tcp://r:1", target.Endpoint);
        Assert.True(target.Draining);
    }

    [Fact]
    public async Task Draining_Descriptor_Is_Not_Selected_For_A_New_Connection()
    {
        var fixture = await FixtureAsync();
        await fixture.PublishPeerAsync("r1", "tcp://r:1", draining: true);

        await fixture.Reconciler.TickAsync();

        Assert.Empty(fixture.Executor.Connected);
    }

    [Fact]
    public async Task Draining_Marker_Is_Monotonic_Across_Subsequent_Renewal()
    {
        var fixture = await FixtureAsync();
        await fixture.Reconciler.TickAsync();

        Assert.True(await fixture.Reconciler.MarkDrainingAsync());
        await fixture.Reconciler.TickAsync();

        var row = Assert.Single(
            (await fixture.Store.ListMeshNodesAsync("play", default)).Items,
            row => row.Rid.Equals(RoutingId.From("local")));
        Assert.Equal(ZLinkFrameworkRuntimeState.Draining, row.State);
        Assert.True(row.DescriptorRevision > 1);
    }

    [Fact]
    public async Task Runtime_weight_change_renews_the_existing_local_row()
    {
        var fixture = await FixtureAsync();
        await fixture.Reconciler.TickAsync();

        fixture.Reconciler.SetLocalWeight(0);
        await fixture.Reconciler.TickAsync();

        var row = Assert.Single(
            (await fixture.Store.ListMeshNodesAsync("play", default)).Items,
            row => row.Rid.Equals(RoutingId.From("local")));
        Assert.Equal(0, row.ChannelWeights["play"]);
        Assert.Equal(fixture.Runtime.OwnerId, row.OwnerId);
    }

    [Fact]
    public async Task RuntimePlacementAndChannelWeightsPublishIncreasingRevisions()
    {
        var fixture = await FixtureAsync();
        await fixture.Reconciler.TickAsync();
        var initial = Assert.Single(
            (await fixture.Store.ListMeshNodesAsync("play", default)).Items,
            row => row.Rid.Equals(RoutingId.From("local")));

        fixture.Reconciler.SetLocalPlacementWeight(10_000);
        await fixture.Reconciler.TickAsync();
        var placement = Assert.Single(
            (await fixture.Store.ListMeshNodesAsync("play", default)).Items,
            row => row.Rid.Equals(RoutingId.From("local")));
        Assert.Equal(10_000, placement.PlacementWeight);
        Assert.True(
            placement.DescriptorRevision > initial.DescriptorRevision);

        fixture.Reconciler.SetLocalWeight(10_000);
        await fixture.Reconciler.TickAsync();
        var channel = Assert.Single(
            (await fixture.Store.ListMeshNodesAsync("play", default)).Items,
            row => row.Rid.Equals(RoutingId.From("local")));
        Assert.Equal(10_000, channel.ChannelWeights["play"]);
        Assert.True(
            channel.DescriptorRevision > placement.DescriptorRevision);
    }

    [Fact]
    public async Task RuntimeActivationConcurrencyPublishesTheCurrentActiveCount()
    {
        var fixture = await FixtureAsync();
        await fixture.Reconciler.TickAsync();
        var initial = Assert.Single(
            (await fixture.Store.ListMeshNodesAsync("play", default)).Items,
            row => row.Rid.Equals(RoutingId.From("local")));

        fixture.Reconciler.SetLocalActivationConcurrency(7);
        await fixture.Reconciler.TickAsync();

        var updated = Assert.Single(
            (await fixture.Store.ListMeshNodesAsync("play", default)).Items,
            row => row.Rid.Equals(RoutingId.From("local")));
        Assert.Equal(7, updated.ActivationConcurrency.Active);
        Assert.Equal(128, updated.ActivationConcurrency.Limit);
        Assert.True(updated.DescriptorRevision > initial.DescriptorRevision);
    }

    [Fact]
    public async Task RouteMeshChannelWeightMutation_UpdatesTheNamedMembershipOnly()
    {
        var fixture = await FixtureAsync(
            channelWeights: new Dictionary<string, int>(StringComparer.Ordinal)
            {
                ["play"] = 100,
                ["orders"] = 100
            });
        await fixture.Reconciler.TickAsync();

        fixture.Reconciler.SetLocalChannelWeight("orders", 300);
        await fixture.Reconciler.TickAsync();

        var updated = Assert.Single(
            (await fixture.Store.ListMeshNodesAsync("play", default)).Items,
            row => row.Rid.Equals(RoutingId.From("local")));
        Assert.Equal(100, updated.ChannelWeights["play"]);
        Assert.Equal(300, updated.ChannelWeights["orders"]);

        Assert.True(await fixture.Reconciler.SetAllLocalChannelWeightsAsync(0));
        var drained = Assert.Single(
            (await fixture.Store.ListMeshNodesAsync("play", default)).Items,
            row => row.Rid.Equals(RoutingId.From("local")));
        Assert.All(drained.ChannelWeights.Values, static weight => Assert.Equal(0, weight));
        Assert.True(drained.DescriptorRevision > updated.DescriptorRevision);
    }

    [Theory]
    [InlineData((int)ZLinkLocationAutoConnectType.DealerMesh, (int)ZLinkLocationRole.Dealer)]
    [InlineData((int)ZLinkLocationAutoConnectType.RouteMesh, (int)ZLinkLocationRole.Router)]
    public void Symmetric_Mesh_Pairwise_Initiator_Connects_From_The_Smaller_Side_Only(
        int typeValue,
        int roleValue)
    {
        var type = (ZLinkLocationAutoConnectType)typeValue;
        var role = (ZLinkLocationRole)roleValue;
        var smaller = Local(type, role, "aa", "tcp://a:1");
        var bigger = Local(type, role, "bb", "tcp://b:1");
        var rowSmaller = Descriptor("aa", "tcp://a:1");
        var rowBigger = Descriptor("bb", "tcp://b:1");

        var fromSmaller = ZLinkAutoConnectPlanner.ComputeDesired(smaller, [rowBigger]);
        var fromBigger = ZLinkAutoConnectPlanner.ComputeDesired(bigger, [rowSmaller]);

        // Only the byte-order smaller routing id dials, so route and dealer
        // meshes share one physical link per peer pair.
        Assert.True(Assert.Single(fromSmaller).Value.InitiatesConnection);
        Assert.False(Assert.Single(fromBigger).Value.InitiatesConnection);
    }

    [Fact]
    public void RouteMesh_Object_Client_Pair_Does_Not_Create_A_Connection()
    {
        var smaller = Local(
            ZLinkLocationAutoConnectType.RouteMesh,
            ZLinkLocationRole.Router,
            "aa",
            "tcp://a:1") with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Client
        };
        var bigger = Local(
            ZLinkLocationAutoConnectType.RouteMesh,
            ZLinkLocationRole.Router,
            "bb",
            "tcp://b:1") with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Client
        };
        var rowSmaller = Descriptor("aa", "tcp://a:1") with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Client,
            ChannelWeights = new Dictionary<string, int>(StringComparer.Ordinal)
        };
        var rowBigger = Descriptor("bb", "tcp://b:1") with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Client,
            ChannelWeights = new Dictionary<string, int>(StringComparer.Ordinal)
        };

        Assert.Empty(ZLinkAutoConnectPlanner.ComputeDesired(smaller, [rowBigger]));
        Assert.Empty(ZLinkAutoConnectPlanner.ComputeDesired(bigger, [rowSmaller]));
        Assert.Equal(1, ZLinkAutoConnectPlanner.CountDiscoveredPeers(smaller, [rowBigger]));
    }

    [Fact]
    public void RouteMesh_Object_Client_Pair_With_Zero_Weight_Server_Channel_Connects()
    {
        var local = Local(
            ZLinkLocationAutoConnectType.RouteMesh,
            ZLinkLocationRole.Router,
            "aa",
            "tcp://a:1") with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Client,
            HasServerChannel = false
        };
        var remote = Descriptor("bb", "tcp://b:1") with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Client,
            ChannelWeights = new Dictionary<string, int>(StringComparer.Ordinal)
            {
                ["orders"] = 0
            }
        };

        Assert.Single(ZLinkAutoConnectPlanner.ComputeDesired(local, [remote]));
    }

    [Fact]
    public void RouteMesh_Object_Client_With_Local_Server_Channel_Connects_To_Object_Client()
    {
        var local = Local(
            ZLinkLocationAutoConnectType.RouteMesh,
            ZLinkLocationRole.Router,
            "aa",
            "tcp://a:1") with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Client,
            HasServerChannel = true
        };
        var remote = Descriptor("bb", "tcp://b:1") with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Client,
            ChannelWeights = new Dictionary<string, int>(StringComparer.Ordinal)
        };

        Assert.Single(ZLinkAutoConnectPlanner.ComputeDesired(local, [remote]));
    }

    [Fact]
    public void RouteMesh_Object_Client_Still_Connects_To_Object_Server()
    {
        var client = Local(
            ZLinkLocationAutoConnectType.RouteMesh,
            ZLinkLocationRole.Router,
            "aa",
            "tcp://a:1") with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Client
        };
        var server = Descriptor("bb", "tcp://b:1") with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            EntrySpotId = "bb-entry"
        };

        Assert.Single(ZLinkAutoConnectPlanner.ComputeDesired(client, [server]));
    }

    [Fact]
    public void RouteMesh_Object_Client_Still_Connects_To_Peer_With_Server_Channel()
    {
        var client = Local(
            ZLinkLocationAutoConnectType.RouteMesh,
            ZLinkLocationRole.Router,
            "aa",
            "tcp://a:1") with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Client
        };
        var defensiveMixedPeer = Descriptor("bb", "tcp://b:1") with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Client
        };

        Assert.Single(
            ZLinkAutoConnectPlanner.ComputeDesired(client, [defensiveMixedPeer]));
    }

    [Fact]
    public void RouteMesh_Object_Server_Pair_Retains_The_Existing_Connection_Rule()
    {
        var smaller = Local(
            ZLinkLocationAutoConnectType.RouteMesh,
            ZLinkLocationRole.Router,
            "aa",
            "tcp://a:1") with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Server
        };
        var bigger = Descriptor("bb", "tcp://b:1") with
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            EntrySpotId = "bb-entry"
        };

        Assert.Single(ZLinkAutoConnectPlanner.ComputeDesired(smaller, [bigger]));
    }

    [Fact]
    public void SpotMesh_All_Members_Dial_While_Only_The_Smaller_Member_Dials_The_Router()
    {
        var smaller = Local(ZLinkLocationAutoConnectType.SpotMesh, ZLinkLocationRole.Spot, "aa", "tcp://a:1");
        var bigger = Local(ZLinkLocationAutoConnectType.SpotMesh, ZLinkLocationRole.Spot, "bb", "tcp://b:1");
        var rowSmaller = Descriptor("aa", "tcp://a:1");
        var rowBigger = Descriptor("bb", "tcp://b:1");

        var fromSmaller = Assert.Single(ZLinkAutoConnectPlanner.ComputeDesired(smaller, [rowBigger])).Value;
        var fromBigger = Assert.Single(ZLinkAutoConnectPlanner.ComputeDesired(bigger, [rowSmaller])).Value;

        Assert.True(fromSmaller.InitiatesConnection);
        Assert.False(fromBigger.InitiatesConnection);
    }

    [Fact]
    public void Endpoint_Less_Mesh_Member_Always_Dials_Dialable_Peers()
    {
        // "zz" sorts after "aa", so the plain initiator rule would tell the
        // endpoint-less member to wait — but nobody can dial it, so it must
        // initiate regardless of the id order.
        var dialOnly = new ZLinkAutoConnectLocal(
            ZLinkLocationAutoConnectType.RouteMesh, "play", ZLinkLocationRole.Router,
            RoutingId.From("zz"), string.Empty);
        var server = Descriptor("aa", "tcp://a:1");

        var desired = ZLinkAutoConnectPlanner.ComputeDesired(dialOnly, [server]);

        Assert.Single(desired);
    }

    [Fact]
    public async Task Reconcile_Connects_New_Targets_And_Disconnects_Vanished_Ones()
    {
        var fixture = await FixtureAsync();
        await fixture.PublishPeerAsync("r1", "tcp://r:1");
        await fixture.PublishPeerAsync("r2", "tcp://r:2");

        await fixture.Reconciler.TickAsync();
        Assert.Equal(2, fixture.Executor.Connected.Count);

        await fixture.RemovePeerAsync("r1");
        await fixture.Reconciler.TickAsync();

        var disconnected = Assert.Single(fixture.Executor.Disconnected);
        Assert.Equal("tcp://r:1", disconnected.Endpoint);
        Assert.Single(fixture.Reconciler.ActiveTargets);
    }

    [Fact]
    public async Task Failed_Connect_Is_Not_Marked_Active_And_Retries_On_The_Next_Tick()
    {
        var fixture = await FixtureAsync();
        await fixture.PublishPeerAsync("r1", "tcp://r:1");
        fixture.Executor.ConnectSucceeds = false;

        await fixture.Reconciler.TickAsync();
        await fixture.Reconciler.TickAsync();

        Assert.Equal(2, fixture.Executor.Connected.Count);
        Assert.Empty(fixture.Reconciler.ActiveTargets);

        fixture.Executor.ConnectSucceeds = true;
        await fixture.Reconciler.TickAsync();
        Assert.Single(fixture.Reconciler.ActiveTargets);
    }

    [Fact]
    public async Task Store_Failure_Retries_The_Last_Desired_Target_Only_Within_Grace()
    {
        var fixture = await FixtureAsync(options =>
            options.StoreFailureGrace = TimeSpan.FromSeconds(3));
        await fixture.PublishPeerAsync("r1", "tcp://r:1");
        fixture.Executor.ConnectSucceeds = false;
        await fixture.Reconciler.TickAsync();
        Assert.Single(fixture.Executor.Connected);

        fixture.PeerResolver.Fail = true;
        await fixture.Reconciler.TickAsync();
        Assert.Equal(2, fixture.Executor.Connected.Count);

        fixture.Time.Advance(TimeSpan.FromSeconds(4));
        await fixture.Reconciler.TickAsync();
        Assert.Equal(2, fixture.Executor.Connected.Count);
    }

    [Fact]
    public async Task Endpoint_Change_For_The_Same_Peer_Key_Is_A_Handover()
    {
        var fixture = await FixtureAsync();
        await fixture.PublishPeerAsync("r1", "tcp://r:1");
        await fixture.Reconciler.TickAsync();

        Assert.Equal(
            ZLinkOwnerLeaseReleaseResult.Released,
            await fixture.Store.ReleaseOwnerLeaseAsync(
                new ZLinkLocationOwnerToken("peer-owner", 2)));
        await fixture.Store.ClaimLiveOwnerAsync(
            "peer-owner",
            TimeSpan.FromMinutes(10));
        await fixture.Store.UpdateMeshNodeAsync(
            Descriptor("r1", "tcp://r:9") with
            {
                LifecycleGeneration = 2,
                DescriptorRevision = 1,
                LeaseGeneration = 3
            },
            ZLinkLocationWriteIntent.Takeover);
        await fixture.Reconciler.TickAsync();

        Assert.Equal(["tcp://r:1", "tcp://r:9"], fixture.Executor.Connected.Select(t => t.Endpoint));
        var dropped = Assert.Single(fixture.Executor.Disconnected);
        Assert.Equal("tcp://r:1", dropped.Endpoint);
    }

    [Fact]
    public async Task Membership_Snapshot_Classifies_Known_And_Unknown_Peers()
    {
        var fixture = await FixtureAsync();

        Assert.Equal(
            ZLinkRouteMeshTargetClassification.Unknown,
            fixture.Reconciler.ClassifyTarget(RoutingId.From("r1")));

        await fixture.PublishPeerAsync("r1", "tcp://r:1");
        await fixture.Reconciler.TickAsync();

        Assert.Equal(
            ZLinkRouteMeshTargetClassification.RequiredNotConnected,
            fixture.Reconciler.ClassifyTarget(RoutingId.From("r1")));
        Assert.Equal(
            ZLinkRouteMeshTargetClassification.Unknown,
            fixture.Reconciler.ClassifyTarget(RoutingId.From("ghost")));

        // Fail-static: a store outage keeps the last snapshot.
        fixture.PeerResolver.Fail = true;
        await fixture.Reconciler.TickAsync();
        Assert.Equal(
            ZLinkRouteMeshTargetClassification.RequiredNotConnected,
            fixture.Reconciler.ClassifyTarget(RoutingId.From("r1")));

        // A successful snapshot replaces fail-static history. Once the row is
        // removed, the rid is an unknown request target rather than a known but
        // disconnected route.
        fixture.PeerResolver.Fail = false;
        await fixture.RemovePeerAsync("r1");
        await fixture.Reconciler.TickAsync();
        Assert.Equal(
            ZLinkRouteMeshTargetClassification.Unknown,
            fixture.Reconciler.ClassifyTarget(RoutingId.From("r1")));
    }

    [Fact]
    public async Task Manual_Mesh_Retains_Observed_Target_After_Row_Removal()
    {
        var fixture = await FixtureAsync(retainRemovedMembers: true);
        await fixture.PublishPeerAsync("r1", "tcp://r:1");
        await fixture.Reconciler.TickAsync();
        await fixture.RemovePeerAsync("r1");
        await fixture.Reconciler.TickAsync();

        Assert.Equal(
            ZLinkRouteMeshTargetClassification.Unknown,
            fixture.Reconciler.ClassifyTarget(RoutingId.From("r1")));
        Assert.True(fixture.Reconciler.HasRetainedPeer(RoutingId.From("r1")));
        Assert.False(fixture.Reconciler.HasRetainedPeer(RoutingId.From("ghost")));
    }

    [Fact]
    public void Membership_Includes_Peers_The_Initiator_Rule_Excludes_From_Dialing()
    {
        // "zz" does not dial "aa" because aa is the ordered initiator.
        // The target remains in the desired set so the inbound side can
        // validate the RID, endpoint and security identity from discovery.
        var local = Local(ZLinkLocationAutoConnectType.RouteMesh, ZLinkLocationRole.Router, "zz", "tcp://z:1");
        var descriptor = Descriptor("aa", "tcp://a:1");

        var desired = ZLinkAutoConnectPlanner.ComputeDesired(local, [descriptor]);

        Assert.False(Assert.Single(desired).Value.InitiatesConnection);
    }

    [Fact]
    public void SpotMesh_Target_Keeps_Peer_RoutingId_For_RidAware_Connect()
    {
        var local = Local(ZLinkLocationAutoConnectType.SpotMesh, ZLinkLocationRole.Spot, "aa", "tcp://a:1");
        var descriptor = Descriptor("zz", "tcp://z:1");

        var desired = ZLinkAutoConnectPlanner.ComputeDesired(local, [descriptor]);

        var target = Assert.Single(desired.Values);
        Assert.Equal(RoutingId.From("zz"), target.NodeRid);
        Assert.Equal("tcp://z:1", target.Endpoint);
    }

    [Fact]
    public async Task Owner_Change_At_The_Same_Endpoint_Refreshes_Without_A_Second_Dial()
    {
        var fixture = await FixtureAsync();
        await fixture.PublishPeerAsync("r1", "tcp://r:1");
        await fixture.Reconciler.TickAsync();

        // The peer restarts: same rid and endpoint, re-claimed by a new
        // owner. The socket transport has already reconnected the broken
        // endpoint, so the reconciler must not race it with another
        // disconnect/connect pair.
        Assert.Equal(
            ZLinkOwnerLeaseReleaseResult.Released,
            await fixture.Store.ReleaseOwnerLeaseAsync(
                new ZLinkLocationOwnerToken("peer-owner", 2)));
        await fixture.Store.ClaimLiveOwnerAsync(
            "peer-owner-2",
            TimeSpan.FromMinutes(10));
        var restarted = Descriptor("r1", "tcp://r:1") with
        {
            OwnerId = "peer-owner-2",
            LeaseGeneration = 3
        };
        await fixture.Store.UpdateMeshNodeAsync(restarted, ZLinkLocationWriteIntent.Takeover);
        await fixture.Reconciler.TickAsync();

        Assert.Equal("tcp://r:1", Assert.Single(fixture.Executor.Connected).Endpoint);
        Assert.Empty(fixture.Executor.Disconnected);
        Assert.Equal("peer-owner-2", Assert.Single(fixture.Reconciler.ActiveTargets).OwnerId);
    }

    [Fact]
    public async Task New_Rid_At_A_Reused_Endpoint_Releases_The_Stale_Auto_Target()
    {
        var fixture = await FixtureAsync();
        await fixture.PublishPeerAsync("old-rid", "tcp://r:1");
        await fixture.Reconciler.TickAsync();

        // A replacement process can publish its new RID before the previous
        // lease row expires. The endpoint must be handed over so the new RID
        // can complete admission instead of leaving the old auto target in
        // the active set.
        fixture.Time.Advance(TimeSpan.FromSeconds(1));
        await fixture.PublishPeerAsync("new-rid", "tcp://r:1");
        await fixture.Reconciler.TickAsync();

        Assert.Equal(
            ["old-rid", "new-rid"],
            fixture.Executor.Connected.Select(target => target.NodeRid.ToString()));
        Assert.Equal("old-rid", Assert.Single(fixture.Executor.Disconnected).NodeRid.ToString());
        var active = Assert.Single(fixture.Reconciler.ActiveTargets);
        Assert.Equal("new-rid", active.NodeRid.ToString());
        Assert.Equal("tcp://r:1", active.Endpoint);
    }

    [Fact]
    public async Task Local_Row_Publish_Rejects_A_Conflicting_Routing_Id()
    {
        var fixture = await FixtureAsync();
        var oldOwner = await fixture.Store.ClaimLiveOwnerAsync(
            "old-local-owner",
            LeaseTtl);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await fixture.Store.UpdateMeshNodeAsync(
                Descriptor("local", "tcp://l:1") with
                {
                    OwnerId = oldOwner.OwnerId,
                    LeaseGeneration = oldOwner.LeaseGeneration
                },
                ZLinkLocationWriteIntent.NewClaim)).Status);
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            async () => await fixture.Reconciler.TickAsync());

        var row = Assert.Single(
            (await fixture.Store.ListMeshNodesAsync("play", default)).Items,
            row => row.Rid.Equals(RoutingId.From("local")));
        Assert.Equal(ZLinkFrameworkErrorKind.AlreadyExists, error.Kind);
        Assert.Equal(oldOwner, new ZLinkLocationOwnerToken(
            row.OwnerId,
            row.LeaseGeneration));
    }

    [Fact]
    public async Task Claimed_Startup_Row_Uses_Renew_Then_Exact_Owner_Cleanup()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var runtime = new ZLinkLocationRuntime(options, store, time);
        await runtime.RenewOwnerLeaseOnceAsync();
        var preparing = Descriptor("local", "tcp://l:1") with
        {
            OwnerId = string.Empty,
            LeaseGeneration = 0,
            State = ZLinkFrameworkRuntimeState.Preparing
        };
        var claim = await runtime.WriteDescriptorAsync(
            preparing,
            ZLinkLocationWriteIntent.NewClaim);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, claim.Status);

        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var resolvers = new ZLinkStoreLocationResolvers(
            store, tracker, new ZLinkObservedLocationGenerations());
        var reconciler = new ZLinkAutoConnectReconciler(
            Local(
                ZLinkLocationAutoConnectType.SpotMesh,
                ZLinkLocationRole.Spot,
                "local",
                "tcp://l:1"),
            preparing,
            runtime,
            resolvers,
            new RecordingExecutor(),
            options,
            time,
            initiallyPublished: true,
            initialStoreGeneration: claim.Generation);

        Assert.True(await reconciler.MarkServingAsync());
        var serving = Assert.Single(
            (await store.ListMeshNodesAsync("play", default)).Items);
        Assert.Equal(ZLinkFrameworkRuntimeState.Serving, serving.State);
        Assert.Equal(runtime.OwnerToken, new ZLinkLocationOwnerToken(
            serving.OwnerId,
            serving.LeaseGeneration));

        await reconciler.ShutdownAsync();
        Assert.Empty((await store.ListMeshNodesAsync("play", default)).Items);
    }

    [Fact]
    public async Task Shutdown_After_Drain_Owner_Cleanup_Does_Not_Require_Released_Lease()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var runtime = new ZLinkLocationRuntime(options, store, time);
        await runtime.StartAsync(RoutingId.From("local"));
        var local = Descriptor("local", "tcp://l:1") with
        {
            OwnerId = string.Empty,
            LeaseGeneration = 0,
            State = ZLinkFrameworkRuntimeState.Serving
        };
        var claim = await runtime.WriteDescriptorAsync(
            local,
            ZLinkLocationWriteIntent.NewClaim);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, claim.Status);

        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var resolvers = new ZLinkStoreLocationResolvers(
            store,
            tracker,
            new ZLinkObservedLocationGenerations());
        var reconciler = new ZLinkAutoConnectReconciler(
            Local(
                ZLinkLocationAutoConnectType.SpotMesh,
                ZLinkLocationRole.Spot,
                "local",
                "tcp://l:1"),
            local,
            runtime,
            resolvers,
            new RecordingExecutor(),
            options,
            time,
            initiallyPublished: true,
            initialStoreGeneration: claim.Generation);

        await runtime.CleanupOwnerForDrainAsync(CancellationToken.None);
        await reconciler.ShutdownAsync();

        Assert.Empty((await store.ListMeshNodesAsync("play", default)).Items);
        await runtime.StopAsync();
    }

    [Fact]
    public async Task Store_Outage_Is_Fail_Static_And_Recovery_Defers_Disconnects()
    {
        var fixture = await FixtureAsync();
        await fixture.PublishPeerAsync("r1", "tcp://r:1");
        await fixture.Reconciler.TickAsync();
        Assert.Single(fixture.Reconciler.ActiveTargets);

        // Outage: the tick keeps the last desired set and cuts nothing.
        fixture.PeerResolver.Fail = true;
        await fixture.Reconciler.TickAsync();
        Assert.Empty(fixture.Executor.Disconnected);
        Assert.Single(fixture.Reconciler.ActiveTargets);

        // Recovery with an empty store: r1 has not re-registered yet. The
        // first tick must not cut it — disconnect diffs wait one heartbeat
        // interval so the mesh does not sweep live peers.
        await fixture.RemovePeerAsync("r1");
        fixture.PeerResolver.Fail = false;
        await fixture.Reconciler.TickAsync();
        Assert.False(fixture.Reconciler.StoreFailed);
        Assert.Empty(fixture.Executor.Disconnected);

        // After the grace the fresh list wins and the vanished peer drops.
        Assert.True(await fixture.Runtime.RenewOwnerLeaseOnceAsync());
        fixture.Time.Advance(TimeSpan.FromSeconds(6));
        await fixture.Reconciler.TickAsync();
        Assert.False(fixture.Reconciler.StoreFailed);
        Assert.Single(fixture.Executor.Disconnected);
    }

    [Fact]
    public async Task Unhealthy_Owner_Lease_Blocks_A_Successful_Empty_List_From_Disconnecting()
    {
        var fixture = await FixtureAsync();
        await fixture.PublishPeerAsync("r1", "tcp://r:1");
        await fixture.Reconciler.TickAsync();
        Assert.Single(fixture.Reconciler.ActiveTargets);

        // A store command that began before an outage can complete after the
        // store resumes. The owner heartbeat remains the recovery authority,
        // so an empty list observed while that lease is unhealthy cannot cut
        // an already admitted transport.
        await fixture.Runtime.StartAsync(RoutingId.From("runtime-node"));
        await fixture.Runtime.StopAsync();
        await fixture.RemovePeerAsync("r1");
        await fixture.Reconciler.TickAsync();

        Assert.True(fixture.Reconciler.StoreFailed);
        Assert.Empty(fixture.Executor.Disconnected);
        Assert.Single(fixture.Reconciler.ActiveTargets);
    }

    [Fact]
    public async Task Hung_AutoConnect_Read_Enters_FailStatic_At_The_Lease_Renew_Bound()
    {
        var fixture = await FixtureAsync(options =>
            options.OwnerLeaseRenewTimeout = TimeSpan.FromMilliseconds(25));
        await fixture.PublishPeerAsync("r1", "tcp://r:1");
        await fixture.Reconciler.TickAsync();
        Assert.Single(fixture.Reconciler.ActiveTargets);

        fixture.PeerResolver.HangUntilCancelled = true;
        var elapsed = System.Diagnostics.Stopwatch.StartNew();
        await fixture.Reconciler.TickAsync();
        elapsed.Stop();

        Assert.True(elapsed.Elapsed < TimeSpan.FromSeconds(1));
        Assert.True(fixture.Reconciler.StoreFailed);
        Assert.Empty(fixture.Executor.Disconnected);
        Assert.Single(fixture.Reconciler.ActiveTargets);
    }

    [Fact]
    public async Task Requested_Cancellation_Is_Not_Classified_As_A_Store_Outage()
    {
        var fixture = await FixtureAsync();
        using var cancellation = new CancellationTokenSource();
        await cancellation.CancelAsync();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await fixture.Reconciler.TickAsync(cancellation.Token));

        Assert.False(fixture.Reconciler.StoreFailed);
    }

    [Fact]
    public async Task StoreFailureGrace_Keeps_Ready_Connections_And_Blocks_New_Outbound_During_Outage()
    {
        var fixture = await FixtureAsync(options => options.StoreFailureGrace = TimeSpan.FromSeconds(3));
        await fixture.PublishPeerAsync("r1", "tcp://r:1");
        await fixture.Reconciler.TickAsync();
        Assert.Equal("tcp://r:1", Assert.Single(fixture.Reconciler.ActiveTargets).Endpoint);

        fixture.PeerResolver.Fail = true;
        await fixture.RemovePeerAsync("r1");
        await fixture.Reconciler.TickAsync();

        Assert.Empty(fixture.Executor.Disconnected);
        Assert.Equal("tcp://r:1", Assert.Single(fixture.Reconciler.ActiveTargets).Endpoint);

        // Even after the grace boundary, the old ready connection stays up.
        // A new peer that appears in the store during the outage is not dialed
        // because fail-static ticks do not compute an expanded desired set.
        await fixture.PublishPeerAsync("r2", "tcp://r:2");
        fixture.Time.Advance(TimeSpan.FromSeconds(4));
        await fixture.Reconciler.TickAsync();

        Assert.Empty(fixture.Executor.Disconnected);
        Assert.Equal(["tcp://r:1"], fixture.Executor.Connected.Select(target => target.Endpoint));
        Assert.Equal("tcp://r:1", Assert.Single(fixture.Reconciler.ActiveTargets).Endpoint);

        // Recovery re-publishes the local row before reading the list, then
        // connects newly visible peers immediately but still defers disconnect
        // diffs for one heartbeat interval.
        fixture.PeerResolver.Fail = false;
        await fixture.Reconciler.TickAsync();

        Assert.False(fixture.Reconciler.StoreFailed);
        Assert.Empty(fixture.Executor.Disconnected);
        Assert.Equal(["tcp://r:1", "tcp://r:2"], fixture.Executor.Connected.Select(target => target.Endpoint));
        Assert.Equal(
            ["tcp://r:1", "tcp://r:2"],
            fixture.Reconciler.ActiveTargets.Select(target => target.Endpoint).Order());

        Assert.True(await fixture.Runtime.RenewOwnerLeaseOnceAsync());
        fixture.Time.Advance(TimeSpan.FromSeconds(6));
        await fixture.Reconciler.TickAsync();

        Assert.False(fixture.Reconciler.StoreFailed);
        Assert.Equal("tcp://r:1", Assert.Single(fixture.Executor.Disconnected).Endpoint);
        Assert.Equal("tcp://r:2", Assert.Single(fixture.Reconciler.ActiveTargets).Endpoint);
    }

    [Fact]
    public async Task Recovery_Republishes_The_Local_Row_Before_Reading_The_List()
    {
        var fixture = await FixtureAsync();
        await fixture.Reconciler.TickAsync();
        var published = (await fixture.Store.ListMeshNodesAsync("play", default)).Items;
        Assert.Single(published);

        // Outage long enough for the local lease to expire and the row to
        // be claimable again.
        fixture.PeerResolver.Fail = true;
        await fixture.Reconciler.TickAsync();
        fixture.Time.Advance(LeaseTtl + TimeSpan.FromSeconds(1));
        await fixture.Runtime.RenewOwnerLeaseOnceAsync();

        fixture.PeerResolver.Fail = false;
        await fixture.Reconciler.TickAsync();

        published = (await fixture.Store.ListMeshNodesAsync("play", default)).Items;
        Assert.Single(published);
        Assert.Equal(fixture.Runtime.OwnerId, published[0].OwnerId);
    }

    [Fact]
    public async Task Dial_Only_Capability_Without_Identity_Still_Connects_And_Never_Advertises()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var runtime = new ZLinkLocationRuntime(options, store, time);
        await runtime.RenewOwnerLeaseOnceAsync();
        await store.ClaimLiveOwnerAsync("peer-owner", TimeSpan.FromMinutes(10));
        await store.UpdateMeshNodeAsync(
            Descriptor("r1", "tcp://r:1"),
            ZLinkLocationWriteIntent.NewClaim);

        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var resolvers = new ZLinkStoreLocationResolvers(
            store, tracker, new ZLinkObservedLocationGenerations());
        var executor = new RecordingExecutor();

        // An EnableClient() dealer has neither a routing id nor an endpoint:
        // it cannot be keyed, so it publishes no row — but it must dial.
        var local = new ZLinkAutoConnectLocal(
            ZLinkLocationAutoConnectType.ClientServer, "play", ZLinkLocationRole.Dealer,
            NodeRid: null, Endpoint: string.Empty);
        var reconciler = new ZLinkAutoConnectReconciler(
            local, localRow: null, runtime, resolvers, executor, options, time);

        await reconciler.TickAsync();

        Assert.Equal("tcp://r:1", Assert.Single(executor.Connected).Endpoint);
        var rows = (await store.ListMeshNodesAsync("play", default)).Items;
        Assert.Single(rows);

        // Shutdown has no row to remove and only tears down connections.
        await reconciler.ShutdownAsync();
        Assert.Single(executor.Disconnected);
    }

    private static ZLinkAutoConnectLocal Local(
        ZLinkLocationAutoConnectType type,
        ZLinkLocationRole role,
        string rid,
        string endpoint) =>
        new(type, "play", role, RoutingId.From(rid), endpoint);

    private static ZLinkMeshNodeDescriptor Descriptor(
        string rid,
        string endpoint,
        string mesh = "play") => new(
        mesh,
        RoutingId.From(rid),
        LifecycleGeneration: 1,
        DescriptorRevision: 1,
        endpoint,
        new Dictionary<string, int>(StringComparer.Ordinal) { [mesh] = 100 },
        SecurityIdentity: ZLinkTransportSecurityIdentity.Plaintext,
        OwnerId: "peer-owner",
        LeaseGeneration: 2,
        UpdatedAt: default)
    {
        State = ZLinkFrameworkRuntimeState.Serving
    };

    private static async Task<ReconcilerFixture> FixtureAsync(
        Action<ZLinkLocationOptions>? configure = null,
        bool retainRemovedMembers = false,
        IReadOnlyDictionary<string, int>? channelWeights = null)
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        configure?.Invoke(options);
        var runtime = new ZLinkLocationRuntime(options, store, time);
        await runtime.RenewOwnerLeaseOnceAsync();
        await store.ClaimLiveOwnerAsync("peer-owner", TimeSpan.FromMinutes(10));

        var tracker = new ZLinkOwnerLeaseTracker(store, options, time);
        var resolvers = new ZLinkStoreLocationResolvers(
            store, tracker, new ZLinkObservedLocationGenerations());
        var failable = new FailablePeerResolver(resolvers);
        var executor = new RecordingExecutor();
        var local = new ZLinkAutoConnectLocal(
            ZLinkLocationAutoConnectType.ClientServer, "play", ZLinkLocationRole.Dealer,
            RoutingId.From("local"), "tcp://l:1");
        var localRow = Descriptor("local", "tcp://l:1") with
        {
            OwnerId = "ignored",
            ChannelWeights = channelWeights is null
                ? new Dictionary<string, int>(StringComparer.Ordinal)
                {
                    ["play"] = 100
                }
                : new Dictionary<string, int>(channelWeights, StringComparer.Ordinal)
        };
        var reconciler = new ZLinkAutoConnectReconciler(
            local, localRow, runtime, failable, executor, options, time,
            retainRemovedMembers: retainRemovedMembers);
        return new ReconcilerFixture(store, runtime, failable, executor, reconciler, time);
    }

    private sealed record ReconcilerFixture(
        ZLinkInMemoryLocationStore Store,
        ZLinkLocationRuntime Runtime,
        FailablePeerResolver PeerResolver,
        RecordingExecutor Executor,
        ZLinkAutoConnectReconciler Reconciler,
        ManualTimeProvider Time)
    {
        public async Task PublishPeerAsync(
            string rid,
            string endpoint,
            bool takeover = false,
            bool draining = false)
        {
            var row = Descriptor(rid, endpoint) with
            {
                State = draining
                    ? ZLinkFrameworkRuntimeState.Draining
                    : ZLinkFrameworkRuntimeState.Serving
            };
            _ = await Store.UpdateMeshNodeAsync(
                row,
                takeover ? ZLinkLocationWriteIntent.Takeover : ZLinkLocationWriteIntent.NewClaim);
        }

        public async Task RemovePeerAsync(string rid)
        {
            var row = Assert.Single(
                (await Store.ListMeshNodesAsync("play", default)).Items,
                candidate => candidate.Rid == RoutingId.From(rid));
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                await Store.RemoveMeshNodeAsync(
                    new ZLinkMeshNodeDescriptorKey("play", row.Rid),
                    new ZLinkLocationOwnerToken(
                        row.OwnerId,
                        row.LeaseGeneration)));
        }
    }

    private sealed class FailablePeerResolver(IZLinkMeshNodeLocationResolver inner)
        : IZLinkMeshNodeLocationResolver
    {
        public bool Fail { get; set; }

        public bool HangUntilCancelled { get; set; }

        public async ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
            string meshName,
            CancellationToken cancellationToken = default)
        {
            if (Fail) throw new InvalidOperationException("store unreachable");
            if (HangUntilCancelled)
                await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            return await inner.ListLiveMeshNodesAsync(meshName, cancellationToken);
        }
    }

    private sealed class RecordingExecutor : IZLinkAutoConnectExecutor
    {
        public List<ZLinkAutoConnectTarget> Connected { get; } = [];

        public List<ZLinkAutoConnectTarget> Disconnected { get; } = [];

        public bool ConnectSucceeds { get; set; } = true;

        public bool DisconnectSucceeds { get; set; } = true;

        public bool Connect(ZLinkAutoConnectTarget target) { Connected.Add(target); return ConnectSucceeds; }

        public bool Disconnect(ZLinkAutoConnectTarget target) { Disconnected.Add(target); return DisconnectSucceeds; }
    }
}
