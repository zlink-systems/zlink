using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    //  Long enough that a join which keeps re-running the reservation ends on
    //  the deadline, short enough that such a regression costs seconds.
    private static readonly TimeSpan JoinDecisionRequestTimeout =
        TimeSpan.FromSeconds(5);

    private static readonly TimeSpan JoinDecisionBudget =
        TimeSpan.FromSeconds(1);

    [Fact]
    public async Task ColdActivation_ExcludesExpiredDescriptorAndSelectsLiveOwner()
    {
        var time = new ManualTimeProvider();
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            relocationStore: new InMemoryRelocationStore(),
            locationTimeProvider: time);
        try
        {
            var store = RequireLocationStore(runtime);
            _ = await PublishCandidateAsync(
                store,
                "expired-cold-owner",
                RoutingId.From("expired-cold-node"),
                TimeSpan.FromSeconds(1),
                placementWeight: 10_000);
            time.Advance(TimeSpan.FromSeconds(2));

            await SendInstanceIntentAsync(runtime, "cold-expired-owner");

            var authority = await ReadAuthorityAsync(
                store,
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("cold-expired-owner"));
            Assert.Equal(
                runtime.Services.GetRequiredService<ZLinkLocationRuntime>()
                    .OwnerToken.OwnerId,
                authority.OwnerId);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ColdActivation_SelectsLiveOwnerDescriptor()
    {
        var time = new ManualTimeProvider();
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            relocationStore: new InMemoryRelocationStore(),
            locationTimeProvider: time);
        try
        {
            var store = RequireLocationStore(runtime);

            await SendInstanceIntentAsync(runtime, "cold-live-owner");

            var authority = await ReadAuthorityAsync(
                store,
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("cold-live-owner"));
            Assert.Equal(
                runtime.Services.GetRequiredService<ZLinkLocationRuntime>()
                    .OwnerToken.OwnerId,
                authority.OwnerId);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ColdActivation_RejectsDescriptorOwnerLeaseWithoutExpiry()
    {
        const string corruptOwnerId = "corrupt-cold-owner";
        var time = new ManualTimeProvider();
        ToggleMissingOwnerLeaseExpiryStore? corruptStore = null;
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            locationStoreWrapper: inner => corruptStore = new(
                inner,
                ZLinkProviderLocationRepository.OwnerKey(corruptOwnerId)),
            relocationStore: new InMemoryRelocationStore(),
            locationTimeProvider: time);
        try
        {
            var store = RequireLocationStore(runtime);
            _ = await PublishCandidateAsync(
                store,
                corruptOwnerId,
                RoutingId.From("corrupt-cold-node"),
                TimeSpan.FromMinutes(1),
                placementWeight: 10_000);
            corruptStore!.Enabled = true;

            var error = await Assert.ThrowsAsync<InvalidDataException>(
                () => SendInstanceIntentAsync(runtime, "cold-corrupt-owner"));

            Assert.Equal(
                "The Location Store owner lease record is invalid.",
                error.Message);
        }
        finally
        {
            if (corruptStore is not null) corruptStore.Enabled = false;
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task EntrySpotPlacement_ExcludesExpiredDescriptorAndSelectsLiveOwner()
    {
        var time = new ManualTimeProvider();
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            locationTimeProvider: time);
        try
        {
            var store = RequireLocationStore(runtime);
            _ = await PublishCandidateAsync(
                store,
                "expired-entry-owner",
                RoutingId.From("expired-entry-node"),
                TimeSpan.FromSeconds(1),
                placementWeight: 10_000);
            time.Advance(TimeSpan.FromSeconds(2));
            var actor = RegisterProbeActor(runtime, actorRef);
            await using var sourceActivation = AttachActorToUserSpot(runtime, actor);

            var result = await runtime.JoinActorEntrySpotAsync(
                actor,
                ZLinkMessage.Empty,
                operationId: null,
                CancellationToken.None);

            Assert.IsType<ZLinkActorJoinResult.Accepted>(result);
            Assert.Equal(default, node.LastActorJoinTargetNodeRid);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task EntrySpotPlacement_SelectsLiveOwnerDescriptor()
    {
        var time = new ManualTimeProvider();
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            locationTimeProvider: time);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            await using var sourceActivation = AttachActorToUserSpot(runtime, actor);

            var result = await runtime.JoinActorEntrySpotAsync(
                actor,
                ZLinkMessage.Empty,
                operationId: null,
                CancellationToken.None);

            Assert.IsType<ZLinkActorJoinResult.Accepted>(result);
            Assert.Equal(default, node.LastActorJoinTargetNodeRid);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task EntrySpotPlacement_RejectsDescriptorOwnerLeaseWithoutExpiry()
    {
        const string corruptOwnerId = "corrupt-entry-owner";
        var time = new ManualTimeProvider();
        ToggleMissingOwnerLeaseExpiryStore? corruptStore = null;
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            locationStoreWrapper: inner => corruptStore = new(
                inner,
                ZLinkProviderLocationRepository.OwnerKey(corruptOwnerId)),
            locationTimeProvider: time);
        try
        {
            var store = RequireLocationStore(runtime);
            _ = await PublishCandidateAsync(
                store,
                corruptOwnerId,
                RoutingId.From("corrupt-entry-node"),
                TimeSpan.FromMinutes(1),
                placementWeight: 10_000);
            corruptStore!.Enabled = true;
            var actor = RegisterProbeActor(runtime, actorRef);
            await using var sourceActivation = AttachActorToUserSpot(runtime, actor);

            var error = await Assert.ThrowsAsync<InvalidDataException>(async () =>
                await runtime.JoinActorEntrySpotAsync(
                    actor,
                    ZLinkMessage.Empty,
                    operationId: null,
                    CancellationToken.None));

            Assert.Equal(
                "The Location Store owner lease record is invalid.",
                error.Message);
        }
        finally
        {
            if (corruptStore is not null) corruptStore.Enabled = false;
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ReadyUserSpot_ExpiredOwnerIsUnavailableAndAuthorityRemains()
    {
        var fixture = await CreateReadyAuthorityFixtureAsync(
            "expired-ready-spot-owner",
            RoutingId.From("expired-ready-spot-node"),
            TimeSpan.FromSeconds(1),
            userSpot: true);
        try
        {
            fixture.Time.Advance(TimeSpan.FromSeconds(2));

            //  The request timeout is long on purpose. A lost owner is a
            //  decision the join makes on the record it just read, so it must
            //  answer well before the deadline; a deadline-shaped Unavailable
            //  would mean the reservation is being re-run against a record
            //  that can never change.
            var started = Stopwatch.GetTimestamp();
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await fixture.Runtime
                    .GetOrCreate(fixture.ObjectId, fixture.StableType)
                    .Timeout(JoinDecisionRequestTimeout)
                    .Async());
            var elapsed = Stopwatch.GetElapsedTime(started);

            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
            Assert.Equal(
                $"User Spot '{fixture.ObjectId}' owner lease is not live.",
                error.Message);
            Assert.Equal(ZLinkRetryAdvice.RetryAfterStateChange, error.RetryAdvice);
            Assert.True(
                elapsed < JoinDecisionBudget,
                $"The join consumed {elapsed} of the {JoinDecisionRequestTimeout} "
                + "request timeout instead of deciding on the owner lease.");
            Assert.Equal(
                fixture.Owner.OwnerId,
                (await ReadAuthorityAsync(fixture.Store, fixture.AuthorityKey)).OwnerId);
        }
        finally
        {
            await fixture.DisposeAsync();
        }
    }

    [Fact]
    public async Task ReadyUserSpot_LiveOwnerReturnsExistingAuthority()
    {
        var fixture = await CreateReadyAuthorityFixtureAsync(
            "live-ready-spot-owner",
            RoutingId.From("live-ready-spot-node"),
            TimeSpan.FromMinutes(1),
            userSpot: true);
        try
        {
            var result = await fixture.Runtime
                .GetOrCreate(fixture.ObjectId, fixture.StableType)
                .Async();

            Assert.Equal(ZLinkSpotCreateState.Existing, result.State);
            Assert.Equal(fixture.ObjectId, result.Spot.SpotId);
        }
        finally
        {
            await fixture.DisposeAsync();
        }
    }

    [Fact]
    public async Task ReadyUserSpot_RejectsOwnerLeaseWithoutExpiry()
    {
        var fixture = await CreateReadyAuthorityFixtureAsync(
            "corrupt-ready-spot-owner",
            RoutingId.From("corrupt-ready-spot-node"),
            TimeSpan.FromMinutes(1),
            userSpot: true,
            corruptExpiry: true);
        try
        {
            var error = await Assert.ThrowsAsync<InvalidDataException>(async () =>
                await fixture.Runtime
                    .GetOrCreate(fixture.ObjectId, fixture.StableType)
                    .Async());

            Assert.Equal(
                "The Location Store owner lease record is invalid.",
                error.Message);
        }
        finally
        {
            await fixture.DisposeAsync();
        }
    }

    [Fact]
    public async Task ReadyActor_ExpiredOwnerIsUnavailableAndAuthorityRemains()
    {
        var fixture = await CreateReadyAuthorityFixtureAsync(
            "expired-ready-actor-owner",
            RoutingId.From("expired-ready-actor-node"),
            TimeSpan.FromSeconds(1),
            userSpot: false);
        try
        {
            fixture.Time.Advance(TimeSpan.FromSeconds(2));
            var manager = new ZLinkActorManagerService(fixture.Runtime);

            var started = Stopwatch.GetTimestamp();
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await manager.GetOrCreate(fixture.ObjectId, fixture.StableType)
                    .InMesh("entry")
                    .Timeout(JoinDecisionRequestTimeout)
                    .Async());
            var elapsed = Stopwatch.GetElapsedTime(started);

            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
            Assert.Equal(
                $"Actor '{fixture.ObjectId}' owner lease is not live.",
                error.Message);
            Assert.Equal(ZLinkRetryAdvice.RetryAfterStateChange, error.RetryAdvice);
            Assert.True(
                elapsed < JoinDecisionBudget,
                $"The join consumed {elapsed} of the {JoinDecisionRequestTimeout} "
                + "request timeout instead of deciding on the owner lease.");
            Assert.Equal(
                fixture.Owner.OwnerId,
                (await ReadAuthorityAsync(fixture.Store, fixture.AuthorityKey)).OwnerId);
        }
        finally
        {
            await fixture.DisposeAsync();
        }
    }

    [Fact]
    public async Task ReadyActor_LiveOwnerReturnsExistingAuthority()
    {
        var fixture = await CreateReadyAuthorityFixtureAsync(
            "live-ready-actor-owner",
            RoutingId.From("live-ready-actor-node"),
            TimeSpan.FromMinutes(1),
            userSpot: false);
        try
        {
            var manager = new ZLinkActorManagerService(fixture.Runtime);

            var result = await manager.GetOrCreate(
                    fixture.ObjectId,
                    fixture.StableType)
                .InMesh("entry")
                .Async();

            var existing = Assert.IsType<ZLinkActorCreateResult.Existing>(result);
            Assert.Equal(fixture.ObjectId, existing.Actor.ActorId);
        }
        finally
        {
            await fixture.DisposeAsync();
        }
    }

    [Fact]
    public async Task ReadyActor_RejectsOwnerLeaseWithoutExpiry()
    {
        var fixture = await CreateReadyAuthorityFixtureAsync(
            "corrupt-ready-actor-owner",
            RoutingId.From("corrupt-ready-actor-node"),
            TimeSpan.FromMinutes(1),
            userSpot: false,
            corruptExpiry: true);
        try
        {
            var manager = new ZLinkActorManagerService(fixture.Runtime);

            var error = await Assert.ThrowsAsync<InvalidDataException>(async () =>
                await manager.GetOrCreate(fixture.ObjectId, fixture.StableType)
                    .InMesh("entry")
                    .Async());

            Assert.Equal(
                "The Location Store owner lease record is invalid.",
                error.Message);
        }
        finally
        {
            await fixture.DisposeAsync();
        }
    }

    [Fact]
    public async Task ActorManager_ResponseLoss_ReturnsStoredCreationTerminal()
    {
        const string actorId = "response-loss-actor";
        const string actorType = "response-loss-probe";
        var targetRid = RoutingId.From("response-loss-node");
        var node = new CapturingSpotNode();
        node.AdmittedMeshPeers.Add(new MeshNodePeer(
            1,
            MeshPeerSource.Discovery,
            MeshPeerState.Admitted,
            targetRid,
            1,
            1,
            "inproc://response-loss-node",
            1,
            0,
            1));
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            preseedActorOwnership: false);
        try
        {
            var store = RequireLocationStore(runtime);
            _ = await PublishCandidateAsync(
                store,
                "response-loss-owner",
                targetRid,
                TimeSpan.FromMinutes(1),
                actorType: actorType);
            ZLinkCreationOperationId? submittedOperation = null;
            ulong submittedObjectGeneration = 0;
            node.ActorCreateRemoteHandler = async (
                _, submittedActorId, stableType, fence, operation, deadlineUnixMs,
                _, _) =>
            {
                submittedOperation = operation;
                submittedObjectGeneration = fence.ObjectGeneration;
                var actor = new ActorRef(
                    submittedActorId,
                    fence.ObjectGeneration,
                    "entry",
                    targetRid);
                var envelope = ZLinkActorCreationTerminalCodec.Encode(
                    new ActorCreateOperationTerminal(
                        RequestResult.Ok,
                        ServiceWireConstants.FrameworkErrorCode.None,
                        new ActorCreateCompletion(ActorCreateResult.Created, actor)),
                    runtime.Registration.Codecs);
                var reservation = new ZLinkObjectReservation(
                    ZLinkActorAuthorityPayloadCodec.AuthorityKey(submittedActorId),
                    fence.ExpectedStoreVersion,
                    fence.ObjectGeneration,
                    fence.AuthorityOwnerGeneration,
                    fence.ReservationId,
                    new ZLinkMeshNodeDescriptorKey("entry", targetRid),
                    fence.TargetNodeGeneration,
                    new ZLinkLocationOwnerToken(
                        fence.TargetOwnerId,
                        fence.TargetOwnerLeaseGeneration));
                var ready = ZLinkActorAuthorityPayloadCodec.Encode(
                    new ZLinkActorAuthorityPayload(
                        ZLinkActorAuthorityState.Ready,
                        stableType,
                        submittedActorId,
                        "entry:test",
                        1,
                        ZLinkSpotKind.Entry,
                        fence.TargetOwnerId,
                        fence.TargetOwnerLeaseGeneration,
                        "entry",
                        targetRid,
                        fence.TargetNodeGeneration));
                var completed = await store.CompleteCreationAsync(
                    reservation,
                    new ZLinkObjectCreationCompletion.Created(
                        ready,
                        new ZLinkCreationTerminalPublication(
                            operation,
                            envelope,
                            DateTimeOffset.FromUnixTimeMilliseconds(
                                    checked((long)deadlineUnixMs))
                                .AddMinutes(1))));
                Assert.IsType<ZLinkObjectCreationCompleteResult.Created>(completed);
                throw new TimeoutException("response lost after terminal storage");
            };

            var manager = new ZLinkActorManagerService(runtime);
            var result = await manager.GetOrCreate(actorId, actorType)
                .InMesh("entry")
                .Async();

            var created = Assert.IsType<ZLinkActorCreateResult.Created>(result);
            Assert.Equal(actorId, created.Actor.ActorId);
            Assert.Equal(submittedObjectGeneration, created.Actor.ObjectGeneration);
            Assert.NotNull(submittedOperation);
            Assert.Equal(node.RoutingId, submittedOperation.Value.SourceNodeRid);
            Assert.Equal(
                node.MeshStatus().LifecycleGeneration,
                submittedOperation.Value.SourceNodeGeneration);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public void ActorManager_OperationIdentity_DoesNotProduceAllZero()
    {
        var operation = ZLinkActorManagerService.CreateOperationId(new byte[16]);

        Assert.Equal(0UL, operation.High);
        Assert.Equal(1UL, operation.Low);
    }

    private static async Task SendInstanceIntentAsync(
        ZLinkFrameworkRuntime runtime,
        string spotId)
    {
        await new ZLinkInstanceSpotSendCall<ProbeRouteMessage>(
                runtime,
                new InstanceSpotIntentAddress(
                    "entry",
                    "Tests.InstanceSpot",
                    spotId),
                new ProbeRouteMessage("activate"))
            .Async();
    }

    private static ZLinkUserSpotActivation AttachActorToUserSpot(
        ZLinkFrameworkRuntime runtime,
        IZLinkActor actor)
    {
        var activation = new ZLinkUserSpotActivation(
            runtime,
            runtime.Services.CreateAsyncScope(),
            new CapturingSpot(),
            "source-user-spot",
            RoutingId.From("entry-node"),
            "entry",
            "entry",
            TimeSpan.FromSeconds(1),
            TimeSpan.FromSeconds(1));
        runtime.GetOrCreateActorState(actor.Context.ActorId).JoinSpot(activation);
        return activation;
    }

    private static IZLinkLocationRepository RequireLocationStore(
        ZLinkFrameworkRuntime runtime) =>
        runtime.Registration.Locations.ResolveStore()
        ?? throw new InvalidOperationException("A Location Store is required.");

    private static async Task<ZLinkLocationOwnerToken> PublishCandidateAsync(
        IZLinkLocationRepository store,
        string ownerId,
        RoutingId routingId,
        TimeSpan leaseTtl,
        int placementWeight = 100,
        string? actorType = null)
    {
        var owner = await store.ClaimLiveOwnerAsync(ownerId, leaseTtl);
        var local = Assert.Single(
            (await store.ListMeshNodesAsync("entry", new ZLinkPageRequest(100))).Items,
            descriptor => descriptor.Rid == RoutingId.From("entry-node"));
        var descriptor = local with
        {
            Rid = routingId,
            Endpoint = $"inproc://{routingId}",
            OwnerId = owner.OwnerId,
            LeaseGeneration = owner.LeaseGeneration,
            DescriptorRevision = 1,
            PlacementWeight = placementWeight,
            ObjectCapabilities = actorType is null
                ? local.ObjectCapabilities
                :
                [
                    ..local.ObjectCapabilities,
                    new ZLinkObjectCapability(
                        ZLinkPlacementObjectKind.Actor,
                        actorType,
                        ZLinkObjectMaintenancePolicyKind.Disabled,
                        false,
                        0)
                ]
        };
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await store.UpdateMeshNodeAsync(
                descriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);
        return owner;
    }

    private static async Task<ZLinkAuthoritySnapshot> ReadAuthorityAsync(
        IZLinkLocationRepository store,
        ZLinkAuthorityKey key) =>
        Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(key)).Snapshot;

    private static async Task<ReadyAuthorityFixture> CreateReadyAuthorityFixtureAsync(
        string ownerId,
        RoutingId ownerNodeRid,
        TimeSpan leaseTtl,
        bool userSpot,
        bool corruptExpiry = false)
    {
        var time = new ManualTimeProvider();
        ToggleMissingOwnerLeaseExpiryStore? corruptStore = null;
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: !userSpot,
            userSpotType: userSpot ? typeof(EmptyUserSpot) : null,
            preseedActorOwnership: false,
            locationStoreWrapper: corruptExpiry
                ? inner => corruptStore = new(
                    inner,
                    ZLinkProviderLocationRepository.OwnerKey(ownerId))
                : null,
            locationTimeProvider: time);
        try
        {
            var store = RequireLocationStore(runtime);
            var owner = await PublishCandidateAsync(
                store,
                ownerId,
                ownerNodeRid,
                leaseTtl,
                actorType: userSpot ? null : "probe");
            var objectId = userSpot
                ? $"ready-spot-{ownerId}"
                : $"ready-actor-{ownerId}";
            var stableType = userSpot ? typeof(EmptyUserSpot).FullName! : "probe";
            var key = userSpot
                ? ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(objectId)
                : ZLinkActorAuthorityPayloadCodec.AuthorityKey(objectId);
            var payload = userSpot
                ? ZLinkUserSpotAuthorityPayloadCodec.Encode(
                    new ZLinkUserSpotAuthorityPayload(
                        ZLinkUserSpotAuthorityState.Ready,
                        stableType,
                        objectId,
                        owner.OwnerId,
                        checked((ulong)owner.LeaseGeneration),
                        "entry",
                        ownerNodeRid,
                        1))
                : ZLinkActorAuthorityPayloadCodec.Encode(
                    new ZLinkActorAuthorityPayload(
                        ZLinkActorAuthorityState.Ready,
                        stableType,
                        objectId,
                        "entry:test",
                        1,
                        ZLinkSpotKind.Entry,
                        owner.OwnerId,
                        checked((ulong)owner.LeaseGeneration),
                        "entry",
                        ownerNodeRid,
                        1));
            await ReserveAndCommitLocalAuthorityAsync(
                store,
                key,
                userSpot
                    ? ZLinkPlacementObjectKind.UserSpot
                    : ZLinkPlacementObjectKind.Actor,
                stableType,
                ownerNodeRid,
                owner,
                payload,
                userSpot
                    ? new ZLinkCapacityVector(
                        0,
                        1,
                        new ZLinkSpotTypeCapacityDelta(
                            ZLinkPlacementObjectKind.UserSpot,
                            stableType,
                            1))
                    : new ZLinkCapacityVector(1, 0, null));
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                await store.RemoveMeshNodeAsync(
                    new ZLinkMeshNodeDescriptorKey("entry", ownerNodeRid),
                    owner));
            if (!userSpot)
            {
                var local = Assert.Single(
                    (await store.ListMeshNodesAsync(
                        "entry",
                        new ZLinkPageRequest(100))).Items,
                    descriptor => descriptor.Rid == RoutingId.From("entry-node"));
                Assert.Equal(
                    ZLinkLocationWriteStatus.Stored,
                    (await store.UpdateMeshNodeAsync(
                        local with
                        {
                            DescriptorRevision = checked(
                                local.DescriptorRevision + 1),
                            ObjectCapabilities =
                            [
                                ..local.ObjectCapabilities,
                                new ZLinkObjectCapability(
                                    ZLinkPlacementObjectKind.Actor,
                                    "probe",
                                    ZLinkObjectMaintenancePolicyKind.Disabled,
                                    false,
                                    0)
                            ]
                        },
                        ZLinkLocationWriteIntent.Renew)).Status);
            }
            if (corruptStore is not null) corruptStore.Enabled = true;
            return new ReadyAuthorityFixture(
                runtime,
                store,
                time,
                owner,
                key,
                objectId,
                stableType,
                corruptStore);
        }
        catch
        {
            if (corruptStore is not null) corruptStore.Enabled = false;
            await runtime.StopAsync(CancellationToken.None);
            throw;
        }
    }

    private sealed record ReadyAuthorityFixture(
        ZLinkFrameworkRuntime Runtime,
        IZLinkLocationRepository Store,
        ManualTimeProvider Time,
        ZLinkLocationOwnerToken Owner,
        ZLinkAuthorityKey AuthorityKey,
        string ObjectId,
        string StableType,
        ToggleMissingOwnerLeaseExpiryStore? CorruptStore) : IAsyncDisposable
    {
        public async ValueTask DisposeAsync()
        {
            if (CorruptStore is not null) CorruptStore.Enabled = false;
            await Runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class ToggleMissingOwnerLeaseExpiryStore(
        IZLinkLocationStore inner,
        ZLinkStoreKey ownerKey) : IZLinkLocationStore
    {
        public bool Enabled { get; set; }

        public async ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default)
        {
            var read = await inner.ReadAsync(key, cancellationToken)
                .ConfigureAwait(false);
            return Enabled
                   && key == ownerKey
                   && read is ZLinkStoreReadResult.Found found
                ? new ZLinkStoreReadResult.Found(
                    found.Value with { ExpiresAt = null })
                : read;
        }

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default) =>
            inner.WriteAsync(request, cancellationToken);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default) =>
            inner.ScanAsync(request, cancellationToken);
    }
}
