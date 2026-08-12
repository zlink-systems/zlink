using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests;

public sealed class ActorJoinRelocationOwnershipRegressionTests
{
    [Fact]
    public async Task
        Command44_Applies_Accepted_Target_Tenure_Without_Downstream_Authority_Read()
    {
        using var fixture = CreateFixture();
        var (route, authenticatedTarget) = await fixture.SealAndCreateCommitAsync();
        var disabledAuthority = new DisabledAuthorityResolver();
        var owner = new ZLinkSessionRelocationBarrierOwner(
            fixture.Runtime,
            disabledAuthority);

        var routed = await owner.RouteAsync(
            route,
            authenticatedTarget,
            CancellationToken.None);

        Assert.Equal(
            ZLinkServiceWireCodec.SessionRelocationRouteResult.Applied,
            routed.Result);
        Assert.Equal(0, disabledAuthority.ReadCalls);
        Assert.True(fixture.Runtime.TryGetSessionActorBinding(
            fixture.ActorId,
            out var committed));
        Assert.Equal(fixture.TargetNode, committed.Route.Ref.NodeRid);
        Assert.Equal(
            fixture.TargetOwnerLeaseGeneration,
            committed.OwnerLeaseGeneration);
    }

    [Fact]
    public async Task
        Original_Pending_Reply_Capability_Survives_Actor_Route_Switch_Exactly_Once()
    {
        using var fixture = CreateFixture();
        const ulong requestId = 41;
        var replyCapability = fixture.Runtime.TrackRemoteSessionActorRequest(
            fixture.ActorId,
            requestId,
            fixture.BindingToken);
        var (route, authenticatedTarget) = await fixture.SealAndCreateCommitAsync();

        var routed = fixture.Runtime.RouteCanonicalSessionActor(
            route,
            authenticatedTarget);
        Assert.Equal(
            ZLinkServiceWireCodec.SessionRelocationRouteResult.Applied,
            routed.Result);

        var replyFrame = new byte[] { 4, 4, 5 };
        await fixture.Runtime.DeliverRemoteActorReplyAsync(
            fixture.ActorId,
            requestId,
            ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
            replyCapability,
            fixture.TargetNode,
            fixture.TargetNode,
            replyFrame,
            CancellationToken.None);
        await fixture.Runtime.DeliverRemoteActorReplyAsync(
            fixture.ActorId,
            requestId,
            ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
            replyCapability,
            fixture.TargetNode,
            fixture.TargetNode,
            replyFrame,
            CancellationToken.None);

        var written = Assert.Single(fixture.Stream.Writes);
        Assert.Equal(replyFrame, written.Payload);
        Assert.Equal(SendFlags.DontWait, written.Flags);
    }

    [Fact]
    public async Task
        Remote_Command44_Shares_One_Authority_Resolution_Then_Uses_The_Apply_Journal()
    {
        using var fixture = CreateFixture();
        var (route, acceptedTarget) = await fixture.SealAndCreateCommitAsync();
        var peerCandidate = acceptedTarget with { OwnerLeaseGeneration = 0 };
        var resolverStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseResolver = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var resolutionCalls = 0;

        async ValueTask<ZLinkSessionRelocationAuthenticatedRoute> ResolveAsync(
            CancellationToken cancellationToken)
        {
            Interlocked.Increment(ref resolutionCalls);
            resolverStarted.TrySetResult();
            await releaseResolver.Task.WaitAsync(cancellationToken);
            return acceptedTarget;
        }

        var first = fixture.Runtime.RouteCanonicalSessionActorAsync(
                route,
                peerCandidate,
                ResolveAsync,
                CancellationToken.None)
            .AsTask();
        await resolverStarted.Task.WaitAsync(TimeSpan.FromSeconds(1));
        var duplicate = fixture.Runtime.RouteCanonicalSessionActorAsync(
                route,
                peerCandidate,
                ResolveAsync,
                CancellationToken.None)
            .AsTask();
        releaseResolver.TrySetResult();

        var concurrent = await Task.WhenAll(first, duplicate);
        Assert.All(
            concurrent,
            response => Assert.Equal(
                ZLinkServiceWireCodec.SessionRelocationRouteResult.Applied,
                response.Result));
        Assert.Equal(1, Volatile.Read(ref resolutionCalls));

        var retry = await fixture.Runtime.RouteCanonicalSessionActorAsync(
            route,
            peerCandidate,
            _ =>
            {
                Interlocked.Increment(ref resolutionCalls);
                return ValueTask.FromException<
                    ZLinkSessionRelocationAuthenticatedRoute>(
                    new InvalidOperationException(
                        "The apply journal must bypass authority resolution."));
            },
            CancellationToken.None);

        Assert.Equal(
            ZLinkServiceWireCodec.SessionRelocationRouteResult.AlreadyApplied,
            retry.Result);
        Assert.Equal(1, Volatile.Read(ref resolutionCalls));
    }

    [Fact]
    public async Task
        A_To_B_Replay_Does_Not_Release_The_B_To_A_Successor_Before_Exact_Command45()
    {
        using var fixture = CreateFixture();
        var (route, _) = await fixture.SealAndCreateCommitAsync();
        const string handoffId = "handoff-a-b-a";
        var handoff = new ZLinkActorHandoffState(
            fixture.ActorId,
            TimeProvider.System);
        handoff.BeginCanonicalMaintenanceImport(handoffId, []);
        handoff.MarkAuthorityCommitted(handoffId, 1, 1);
        handoff.RequireSessionRouteTerminal(handoffId, route);
        Assert.Empty(handoff.PrepareCanonicalMaintenanceReplay(handoffId));
        Assert.True(handoff.TryCompleteTransferredActorReplay(handoffId));

        var successor = handoff.BeginDeferredJoinCapture();
        Assert.NotNull(successor);
        Assert.False(successor.IsCompleted);
        Assert.Throws<InvalidDataException>(() =>
            handoff.ObserveSessionRouteTerminal(
                handoffId,
                route with { SenderRole = 1 }));
        Assert.False(successor.IsCompleted);

        Assert.True(handoff.ObserveSessionRouteTerminal(handoffId, route));
        await successor.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.False(handoff.ObserveSessionRouteTerminal(handoffId, route));
    }

    [Fact]
    public void
        Frozen_Command44_Rejects_Conflicting_Command45_Before_Route_Promotion()
    {
        const string actorId = "actor-frozen-command-45";
        const string handoffId = "handoff-frozen-command-45";
        var sourceNode = RoutingId.From("source-frozen-command-45");
        var targetNode = RoutingId.From("target-frozen-command-45");
        var sessionNode = RoutingId.From("session-frozen-command-45");
        var sessionRid = RoutingId.From("session-rid-frozen-command-45");
        var state = new ZLinkActorRuntimeState(actorId);
        state.Handoff.BeginCanonicalMaintenanceImport(handoffId, []);
        state.StageRelocationSessionRoute(
            handoffId,
            new ZLinkRemoteActorBoundSessionRoute(
                sessionNode,
                sessionRid,
                "binding-frozen-command-45",
                BindingGeneration: 4,
                ObjectGeneration: 7,
                AuthorityOwnerGeneration: 11,
                MeshName: "actors",
                TargetNodeGeneration: 3,
                OwnerLeaseGeneration: 17,
                SessionOwnerNodeGeneration: 5,
                AcceptedHighWater: 9,
                SessionOwnerId: "session-owner-frozen-command-45",
                SessionOwnerLeaseGeneration: 19),
            ZLinkSessionRelocationContext.Create(
                Guid.Parse("a80e4d98-595e-4d85-b788-938253758e35"),
                "source-owner-frozen-command-45",
                17,
                sourceNode,
                3,
                "store-frozen-command-45"));
        state.MarkRelocationSessionAuthorityCommitted(
            handoffId,
            new ZLinkBackendActorRef(targetNode, actorId, 7),
            targetAuthorityOwnerGeneration: 12,
            ZLinkMeshName.FromBoundary("actors", "meshName"),
            targetNodeGeneration: 4,
            targetOwnerLeaseGeneration: 23);
        state.RequireRelocationSessionRouteTerminal(handoffId);

        state.RecordRelocatedSessionAccepted(sessionRid, 13);
        var command44 = state.FreezeRelocationSessionRouteTerminal(handoffId);
        Assert.Equal((ulong)13, command44.Route.ReplayedHighWater);

        state.RecordRelocatedSessionAccepted(sessionRid, 17);
        var conflicting = CreateRouted(command44, acceptedHighWater: 17);
        Assert.Throws<InvalidDataException>(() =>
            state.CompleteRelocationSessionRoute(
                handoffId,
                command44,
                conflicting));
        Assert.True(state.TryGetCommittedRelocationSessionRoute(
            handoffId,
            out _));
        Assert.False(state.TryGetBoundSession(out _));

        var exact = CreateRouted(command44, acceptedHighWater: 13);
        Assert.True(state.CompleteRelocationSessionRoute(
            handoffId,
            command44,
            exact));
        Assert.False(state.TryGetCommittedRelocationSessionRoute(
            handoffId,
            out _));
        Assert.True(state.TryGetBoundSession(out var promoted));
        Assert.Equal((ulong)13, promoted.AcceptedHighWater);
    }

    [Fact]
    public async Task
        Session_Binding_Aggregate_Releases_Bounded_Fifo_Only_On_Exact_Command44()
    {
        using var fixture = CreateFixture();
        var stream = new RecordingStream(
            RoutingId.From("session-fifo-regression"));
        var context = new ZLinkSessionContext(
            fixture.Runtime,
            stream,
            new EmptySessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        var table = new ZLinkSessionActorBindingTable(
            TimeSpan.FromMinutes(1),
            maxRetainedOutbound: 2);
        const string actorId = "actor-fifo-regression";
        const string bindingToken = "binding-fifo-regression";
        var sourceNode = RoutingId.From("source-fifo-regression");
        var targetNode = RoutingId.From("target-fifo-regression");
        var sourceRoute = ZLinkSessionBindingRoute.Create(
            new ActorRef(actorId, 5, "actors", sourceNode),
            "actors",
            2,
            11,
            17);
        table.Bind(
            ZLinkActorId.FromBoundary(actorId, nameof(actorId)),
            context,
            bindingToken,
            new ZLinkSessionActor(
                context,
                actorId,
                stream.RoutingId!.Value,
                bindingToken),
            bindingGeneration: 6,
            sourceRoute,
            sessionOwnerNodeGeneration: 7,
            sessionOwnerNodeRid: RoutingId.From("owner-fifo-regression"),
            sessionOwnerId: "owner-fifo-regression",
            sessionOwnerLeaseGeneration: 8);
        var seal = CreateSeal(
            actorId,
            stream.RoutingId.Value,
            sourceNode,
            sourceRoute,
            bindingGeneration: 6,
            sessionOwnerNodeGeneration: 7,
            sessionOwnerLeaseGeneration: 8,
            relocationHigh: 307,
            relocationLow: 311,
            sessionOwnerNode: RoutingId.From("owner-fifo-regression"),
            sessionOwnerId: "owner-fifo-regression");
        var sealedRecord = await table.SealCanonicalRouteAsync(
            seal,
            CancellationToken.None);
        var tenure = new ZLinkSessionOutboundTenure(
            actorId,
            5,
            "actors",
            targetNode,
            3,
            12,
            23,
            bindingToken,
            6,
            7,
            stream.RoutingId.Value);
        var proof = new ZLinkSessionOutboundTenureProof(
            tenure,
            "target-owner-fifo-regression");
        var first = table.AdmitOutbound(tenure, proof, [1]);
        var second = table.AdmitOutbound(tenure, firstProof: null, [2]);
        var overflow = table.AdmitOutbound(tenure, firstProof: null, [3]);
        Assert.Equal(ZLinkSessionOutboundAdmissionKind.Retained, first.Kind);
        Assert.Equal(ZLinkSessionOutboundAdmissionKind.Retained, second.Kind);
        Assert.Equal(
            ZLinkSessionOutboundAdmissionKind.Backpressured,
            overflow.Kind);
        Assert.Empty(stream.Writes);

        var command44 = new ZLinkServiceWireCodec.SessionRelocationRouteRecord(
            seal.RelocationId,
            seal.Coordinator,
            2,
            seal.Actor.Actor,
            seal.Session,
            ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Commit(
                11,
                12,
                targetNode,
                3,
                sealedRecord.LastAcceptedSessionSequence));
        table.RouteCanonical(
            command44,
            new ZLinkSessionRelocationAuthenticatedRoute(
                targetNode,
                3,
                "actors",
                12,
                23));

        Assert.Equal(new byte[] { 1 }, stream.Writes[0].Payload);
        Assert.Equal(new byte[] { 2 }, stream.Writes[1].Payload);
        Assert.Equal(
            ZLinkSessionOutboundDelivery.Delivered,
            await first.Capability!.Completion);
        Assert.Equal(
            ZLinkSessionOutboundDelivery.Delivered,
            await second.Capability!.Completion);
    }

    [Fact]
    public async Task
        Outstanding_Apply_Journal_Rejects_Capacity_Before_Second_Seal_Mutation()
    {
        using var fixture = CreateFixture();
        var table = new ZLinkSessionActorBindingTable(
            TimeSpan.FromMinutes(1),
            maxCanonicalRouteApplications: 1);
        var first = BindCanonicalActor(
            table,
            fixture.Runtime,
            "actor-journal-first",
            "binding-journal-first",
            "session-journal-first",
            "source-journal-first",
            "owner-journal-first");
        var second = BindCanonicalActor(
            table,
            fixture.Runtime,
            "actor-journal-second",
            "binding-journal-second",
            "session-journal-second",
            "source-journal-second",
            "owner-journal-second");
        await table.SealCanonicalRouteAsync(first.Seal, CancellationToken.None);

        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            table.SealCanonicalRouteAsync(
                    second.Seal,
                    CancellationToken.None)
                .AsTask());
        Assert.Equal(ZLinkFrameworkErrorKind.Rejected, failure.Kind);
        Assert.True(table.TryAccept(
            second.ActorId,
            second.BindingToken,
            out var accepted));
        Assert.Equal((ulong)1, accepted);
        table.CompleteAccepted(second.ActorId, second.BindingToken);
    }

    [Fact]
    public async Task
        Generation_Reset_Closes_Application_Epoch_Before_Cancel_And_Reopens_After_Table_Reset()
    {
        using var fixture = CreateFixture();
        var registration = new ZLinkFrameworkRegistration
        {
            DefaultRequestTimeout = TimeSpan.FromSeconds(1)
        };
        var states = new Dictionary<string, ZLinkActorRuntimeState>(
            StringComparer.Ordinal);
        var coordinator = new ZLinkActorBoundSessionCoordinator(
            actorId => states.TryGetValue(actorId, out var state)
                ? state
                : states[actorId] = new ZLinkActorRuntimeState(actorId),
            static () => null,
            static _ => null,
            registration,
            static () => CancellationToken.None);
        var binding = BindCoordinatorActor(
            coordinator,
            fixture.Runtime,
            "actor-reset-epoch",
            "binding-reset-epoch",
            "session-reset-epoch",
            "source-reset-epoch",
            "owner-reset-epoch");
        var sealedRecord = await coordinator.SealCanonicalSessionRouteAsync(
            binding.Seal,
            CancellationToken.None);
        var targetNode = RoutingId.From("target-reset-epoch");
        var command44 = new ZLinkServiceWireCodec.SessionRelocationRouteRecord(
            binding.Seal.RelocationId,
            binding.Seal.Coordinator,
            2,
            binding.Seal.Actor.Actor,
            binding.Seal.Session,
            ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Commit(
                11,
                12,
                targetNode,
                3,
                sealedRecord.LastAcceptedSessionSequence));
        var accepted = new ZLinkSessionRelocationAuthenticatedRoute(
            targetNode,
            3,
            "actors",
            12,
            23);
        var peerCandidate = accepted with { OwnerLeaseGeneration = 0 };
        var resolverStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var resetCancellationEntered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseReset = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);

        async ValueTask<ZLinkSessionRelocationAuthenticatedRoute> ResolveAsync(
            CancellationToken cancellationToken)
        {
            using var cancellationRegistration = cancellationToken.Register(() =>
            {
                resetCancellationEntered.TrySetResult();
                releaseReset.Task.GetAwaiter().GetResult();
            });
            resolverStarted.TrySetResult();
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            return accepted;
        }

        var beforeReset = coordinator.RouteCanonicalSessionAsync(
                command44,
                peerCandidate,
                ResolveAsync,
                CancellationToken.None)
            .AsTask();
        await resolverStarted.Task.WaitAsync(TimeSpan.FromSeconds(1));

        var reset = Task.Run(coordinator.ResetGeneration);
        await resetCancellationEntered.Task.WaitAsync(TimeSpan.FromSeconds(1));
        var duringReset = Assert.Throws<ZLinkFrameworkException>(() =>
            coordinator.RouteCanonicalSession(command44, accepted));
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, duringReset.Kind);

        releaseReset.TrySetResult();
        await reset.WaitAsync(TimeSpan.FromSeconds(1));
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => beforeReset);

        var afterReset = coordinator.RouteCanonicalSession(command44, accepted);
        Assert.Equal(
            ZLinkServiceWireCodec.SessionRelocationRouteResult
                .SessionOrBindingClosed,
            afterReset.Result);
    }

    [Fact]
    public async Task
        Remote_Future_Tenure_Without_Store_Is_Rejected_Without_Aggregate_Mutation()
    {
        using var fixture = CreateFixture();
        const string actorId = "actor-remote-no-store";
        var targetNode = RoutingId.From("target-remote-no-store");
        var registration = new ZLinkFrameworkRegistration
        {
            DefaultRequestTimeout = TimeSpan.FromSeconds(1)
        };
        var states = new Dictionary<string, ZLinkActorRuntimeState>(
            StringComparer.Ordinal);
        var coordinator = new ZLinkActorBoundSessionCoordinator(
            id => states.TryGetValue(id, out var state)
                ? state
                : states[id] = new ZLinkActorRuntimeState(id),
            static () => null,
            static _ => null,
            registration,
            static () => CancellationToken.None,
            static () => null);
        var binding = BindCoordinatorActor(
            coordinator,
            fixture.Runtime,
            actorId,
            "binding-remote-no-store",
            "session-remote-no-store",
            "source-remote-no-store",
            "owner-remote-no-store");
        var sealedRecord = await coordinator.SealCanonicalSessionRouteAsync(
            binding.Seal,
            CancellationToken.None);
        var relay = new ZLinkRemoteSessionPushRelay(
            actorId,
            5,
            "actors",
            targetNode.ToHex(),
            3,
            12,
            23,
            binding.BindingToken,
            6,
            7,
            binding.Stream.RoutingId!.Value.ToHex(),
            [1]);

        var rejected = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            coordinator.AdmitRemoteSessionFrameAsync(
                    relay,
                    [1],
                    targetNode,
                    CancellationToken.None)
                .AsTask());

        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, rejected.Kind);
        Assert.Empty(binding.Stream.Writes);

        var command44 = new ZLinkServiceWireCodec.SessionRelocationRouteRecord(
            binding.Seal.RelocationId,
            binding.Seal.Coordinator,
            2,
            binding.Seal.Actor.Actor,
            binding.Seal.Session,
            ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Commit(
                11,
                12,
                targetNode,
                3,
                sealedRecord.LastAcceptedSessionSequence));
        var routed = coordinator.RouteCanonicalSession(
            command44,
            new ZLinkSessionRelocationAuthenticatedRoute(
                targetNode,
                3,
                "actors",
                12,
                23));
        Assert.Equal(
            ZLinkServiceWireCodec.SessionRelocationRouteResult.Applied,
            routed.Result);
        Assert.Empty(binding.Stream.Writes);

        var afterCommit = await coordinator.AdmitRemoteSessionFrameAsync(
            relay with { Frame = [2] },
            [2],
            targetNode,
            CancellationToken.None);
        Assert.Equal(
            ZLinkActorBoundSessionCoordinator.RemotePushDelivery.Delivered,
            afterCommit);
        Assert.Equal(
            new byte[] { 2 },
            binding.Stream.Writes.Select(write => write.Payload[0]));
    }

    [Fact]
    public async Task
        First_Remote_Future_Tenure_Reads_Store_Once_Then_Uses_Immutable_Aggregate_Proof()
    {
        using var fixture = CreateFixture();
        const string actorId = "actor-remote-proof";
        var targetNode = RoutingId.From("target-remote-proof");
        var authority = new BlockingReadyAuthorityResolver(
            actorId,
            objectGeneration: 5,
            authorityOwnerGeneration: 12,
            ownerId: "target-owner-remote-proof",
            ownerLeaseGeneration: 23,
            meshName: "actors",
            targetNode,
            targetNodeGeneration: 3);
        var registration = new ZLinkFrameworkRegistration
        {
            DefaultRequestTimeout = TimeSpan.FromSeconds(1)
        };
        var states = new Dictionary<string, ZLinkActorRuntimeState>(
            StringComparer.Ordinal);
        var coordinator = new ZLinkActorBoundSessionCoordinator(
            id => states.TryGetValue(id, out var state)
                ? state
                : states[id] = new ZLinkActorRuntimeState(id),
            static () => null,
            static _ => null,
            registration,
            static () => CancellationToken.None,
            () => authority);
        var binding = BindCoordinatorActor(
            coordinator,
            fixture.Runtime,
            actorId,
            "binding-remote-proof",
            "session-remote-proof",
            "source-remote-proof",
            "owner-remote-proof");
        var sealedRecord = await coordinator.SealCanonicalSessionRouteAsync(
            binding.Seal,
            CancellationToken.None);
        var relay = new ZLinkRemoteSessionPushRelay(
            actorId,
            5,
            "actors",
            targetNode.ToHex(),
            3,
            12,
            23,
            binding.BindingToken,
            6,
            7,
            binding.Stream.RoutingId!.Value.ToHex(),
            [1]);
        var first = coordinator.AdmitRemoteSessionFrameAsync(
                relay,
                [1],
                targetNode,
                CancellationToken.None)
            .AsTask();
        await authority.ReadStarted.WaitAsync(TimeSpan.FromSeconds(1));
        var second = coordinator.AdmitRemoteSessionFrameAsync(
                relay with { Frame = [2] },
                [2],
                targetNode,
                CancellationToken.None)
            .AsTask();
        await Task.Yield();
        Assert.Equal(1, authority.ReadCalls);
        authority.ReleaseRead();
        await Task.Delay(TimeSpan.FromMilliseconds(25));
        Assert.False(first.IsCompleted);
        Assert.False(second.IsCompleted);

        var command44 = new ZLinkServiceWireCodec.SessionRelocationRouteRecord(
            binding.Seal.RelocationId,
            binding.Seal.Coordinator,
            2,
            binding.Seal.Actor.Actor,
            binding.Seal.Session,
            ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Commit(
                11,
                12,
                targetNode,
                3,
                sealedRecord.LastAcceptedSessionSequence));
        coordinator.RouteCanonicalSession(
            command44,
            new ZLinkSessionRelocationAuthenticatedRoute(
                targetNode,
                3,
                "actors",
                12,
                23));
        Assert.Equal(
            ZLinkActorBoundSessionCoordinator.RemotePushDelivery.Delivered,
            await first.WaitAsync(TimeSpan.FromSeconds(1)));
        Assert.Equal(
            ZLinkActorBoundSessionCoordinator.RemotePushDelivery.Delivered,
            await second.WaitAsync(TimeSpan.FromSeconds(1)));

        var afterCommit = await coordinator.AdmitRemoteSessionFrameAsync(
            relay with { Frame = [3] },
            [3],
            targetNode,
            CancellationToken.None);
        Assert.Equal(
            ZLinkActorBoundSessionCoordinator.RemotePushDelivery.Delivered,
            afterCommit);
        Assert.Equal(1, authority.ReadCalls);
        Assert.Equal(
            new byte[] { 1, 2, 3 },
            binding.Stream.Writes.Select(write => write.Payload[0]));
    }

    [Fact]
    public async Task
        BarrierOwner_LeaseZero_Command44_Reuses_Command36_Proof_Without_Store_Reread()
    {
        const string actorId = "actor-barrier-proof";
        const string meshName = "actors";
        const string bindingToken = "binding-barrier-proof";
        const ulong targetNodeGeneration = 3;
        var providerStore = new BlockingCountingLocationStore(
            new ZLinkInMemoryProviderLocationStore());
        var registration = new ZLinkFrameworkRegistration
        {
            DefaultRequestTimeout = TimeSpan.FromSeconds(1)
        };
        registration.Locations.StoreInstance = providerStore;
        var services = new ServiceCollection();
        services.AddSingleton(registration);
        using var serviceProvider = services.BuildServiceProvider();
        var runtime = new ZLinkFrameworkRuntime(
            serviceProvider,
            null!,
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                serviceProvider.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var store = registration.Locations.ResolveStore()!;
        var targetNode = RoutingId.From("target-barrier-proof");
        var targetOwner = await store.ClaimLiveOwnerAsync(
            "target-owner-barrier-proof",
            TimeSpan.FromMinutes(5));
        _ = Assert.IsType<ZLinkAuthoritySnapshot>(
            await AuthorityLocationTestFixture.PublishActorAsync(
                store,
                new ZLinkResolvedActorLocation(
                    meshName,
                    "actor-barrier-proof-primer",
                    "player",
                    new ActorRef(
                        "actor-barrier-proof-primer",
                        1,
                        meshName,
                        targetNode),
                    targetNode,
                    targetNodeGeneration,
                    "entry:barrier-proof",
                    1,
                    ZLinkSpotKind.Entry,
                    0,
                    targetOwner.OwnerId,
                    targetOwner.LeaseGeneration,
                    default,
                    0)));
        var targetAuthority = Assert.IsType<ZLinkAuthoritySnapshot>(
            await AuthorityLocationTestFixture.PublishActorAsync(
                store,
                new ZLinkResolvedActorLocation(
                    meshName,
                    actorId,
                    "player",
                    new ActorRef(actorId, 1, meshName, targetNode),
                    targetNode,
                    targetNodeGeneration,
                    "entry:barrier-proof",
                    1,
                    ZLinkSpotKind.Entry,
                    0,
                    targetOwner.OwnerId,
                    targetOwner.LeaseGeneration,
                    default,
                    0)));
        Assert.True(targetAuthority.AuthorityOwnerGeneration > 1);

        var sourceNode = RoutingId.From("source-barrier-proof");
        var sessionOwnerNode = RoutingId.From("owner-barrier-proof");
        var sessionRid = RoutingId.From("session-barrier-proof");
        var stream = new RecordingStream(sessionRid);
        var context = new ZLinkSessionContext(
            runtime,
            stream,
            new EmptySessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        var sourceRoute = ZLinkSessionBindingRoute.Create(
            new ActorRef(
                actorId,
                targetAuthority.ObjectGeneration,
                meshName,
                sourceNode),
            meshName,
            2,
            targetAuthority.AuthorityOwnerGeneration - 1,
            17);
        runtime.BindSessionActor(
            actorId,
            context,
            bindingToken,
            new ZLinkSessionActor(
                context,
                actorId,
                sessionRid,
                bindingToken),
            6,
            sourceRoute,
            7,
            sessionOwnerNode,
            "session-owner-barrier-proof",
            8);
        var seal = CreateSeal(
            actorId,
            sessionRid,
            sourceNode,
            sourceRoute,
            6,
            7,
            8,
            461,
            463,
            sessionOwnerNode,
            "session-owner-barrier-proof");
        var sealedRecord = await runtime.SealCanonicalSessionActorRouteAsync(
            seal,
            CancellationToken.None);
        var targetOwnerLease = checked((ulong)targetOwner.LeaseGeneration);
        var relay = new ZLinkRemoteSessionPushRelay(
            actorId,
            targetAuthority.ObjectGeneration,
            meshName,
            targetNode.ToHex(),
            targetNodeGeneration,
            targetAuthority.AuthorityOwnerGeneration,
            targetOwnerLease,
            bindingToken,
            6,
            7,
            sessionRid.ToHex(),
            [1]);

        providerStore.ResetAndBlockNextAuthorityRead();
        var first = runtime.DeliverRemoteSessionPushAsync(
                relay,
                [1],
                targetNode,
                CancellationToken.None)
            .AsTask();
        await providerStore.ReadStarted.WaitAsync(TimeSpan.FromSeconds(1));
        var second = runtime.DeliverRemoteSessionPushAsync(
                relay with { Frame = [2] },
                [2],
                targetNode,
                CancellationToken.None)
            .AsTask();
        Assert.Equal(1, providerStore.AuthorityReadCalls);
        providerStore.ReleaseRead();
        await providerStore.ReadCompleted.WaitAsync(TimeSpan.FromSeconds(1));
        await Task.Delay(TimeSpan.FromMilliseconds(25));
        Assert.False(first.IsCompleted);
        Assert.False(second.IsCompleted);
        Assert.Equal(1, providerStore.AuthorityReadCalls);
        providerStore.DisableReads();

        var command44 = new ZLinkServiceWireCodec.SessionRelocationRouteRecord(
            seal.RelocationId,
            seal.Coordinator,
            2,
            seal.Actor.Actor,
            seal.Session,
            ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Commit(
                sourceRoute.AuthorityOwnerGeneration,
                targetAuthority.AuthorityOwnerGeneration,
                targetNode,
                targetNodeGeneration,
                sealedRecord.LastAcceptedSessionSequence));
        var leaseZero = new ZLinkSessionRelocationAuthenticatedRoute(
            targetNode,
            targetNodeGeneration,
            meshName,
            targetAuthority.AuthorityOwnerGeneration,
            0);
        var conflicting = command44 with
        {
            Route = command44.Route with
            {
                TargetAuthorityOwnerGeneration = checked(
                    targetAuthority.AuthorityOwnerGeneration + 1)
            }
        };
        var conflictingCandidate = leaseZero with
        {
            AuthorityOwnerGeneration =
                conflicting.Route.TargetAuthorityOwnerGeneration
        };
        var barrier = new ZLinkSessionRelocationBarrierOwner(runtime, store);

        await Assert.ThrowsAsync<IOException>(() =>
            barrier.RouteAsync(
                    conflicting,
                    conflictingCandidate,
                    CancellationToken.None)
                .AsTask());
        Assert.Empty(stream.Writes);
        Assert.False(first.IsCompleted);
        Assert.False(second.IsCompleted);
        Assert.True(runtime.TryGetSessionActorBinding(
            actorId,
            out var unchanged));
        Assert.Equal(sourceNode, unchanged.Route.Ref.NodeRid);
        Assert.Equal(
            sourceRoute.AuthorityOwnerGeneration,
            unchanged.AuthorityOwnerGeneration);

        var routed = await barrier.RouteAsync(
            command44,
            leaseZero,
            CancellationToken.None);

        Assert.Equal(
            ZLinkServiceWireCodec.SessionRelocationRouteResult.Applied,
            routed.Result);
        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(1));
        Assert.Equal(1, providerStore.AuthorityReadCalls);
        Assert.Equal(
            new byte[] { 1, 2 },
            stream.Writes.Select(write => write.Payload[0]));
    }

    private static ZLinkServiceWireCodec.SessionRelocationRoutedRecord
        CreateRouted(
            ZLinkServiceWireCodec.SessionRelocationRouteRecord command44,
            ulong acceptedHighWater) =>
        new(
            command44.RelocationId,
            command44.Coordinator,
            command44.Actor,
            command44.Session,
            command44.Route.Action,
            ZLinkServiceWireCodec.SessionRelocationRouteResult.Applied,
            command44.Route.TargetAuthorityOwnerGeneration,
            acceptedHighWater);

    private static ZLinkServiceWireCodec.SessionRelocationSealRecord CreateSeal(
        string actorId,
        RoutingId sessionRid,
        RoutingId sourceNode,
        ZLinkSessionBindingRoute sourceRoute,
        ulong bindingGeneration,
        ulong sessionOwnerNodeGeneration,
        ulong sessionOwnerLeaseGeneration,
        ulong relocationHigh,
        ulong relocationLow,
        RoutingId sessionOwnerNode,
        string sessionOwnerId) =>
        new(
            new ZLinkServiceWireCodec.RelocationWireId(
                relocationHigh,
                relocationLow),
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                $"source-owner-{actorId}",
                sourceRoute.OwnerLeaseGeneration,
                sourceNode,
                sourceRoute.TargetNodeGeneration,
                $"store-{actorId}"),
            1,
            new ZLinkServiceWireCodec.SessionActorRouteFenceRecord(
                new ZLinkServiceWireCodec.SessionActorIdentityRecord(
                    actorId,
                    sourceRoute.Ref.ObjectGeneration),
                sourceNode,
                sourceRoute.TargetNodeGeneration,
                sourceRoute.AuthorityOwnerGeneration,
                sourceRoute.OwnerLeaseGeneration),
            new ZLinkServiceWireCodec.SessionOwnerFenceRecord(
                sessionOwnerNode,
                sessionOwnerNodeGeneration,
                sessionOwnerId,
                sessionOwnerLeaseGeneration,
                sessionRid,
                bindingGeneration));

    private static CanonicalBindingFixture BindCanonicalActor(
        ZLinkSessionActorBindingTable table,
        ZLinkFrameworkRuntime runtime,
        string actorId,
        string bindingToken,
        string sessionName,
        string sourceName,
        string ownerName)
    {
        var sessionRid = RoutingId.From(sessionName);
        var sourceNode = RoutingId.From(sourceName);
        var ownerNode = RoutingId.From(ownerName);
        var stream = new RecordingStream(sessionRid);
        var context = new ZLinkSessionContext(
            runtime,
            stream,
            new EmptySessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        var sourceRoute = ZLinkSessionBindingRoute.Create(
            new ActorRef(actorId, 5, "actors", sourceNode),
            "actors",
            2,
            11,
            17);
        table.Bind(
            ZLinkActorId.FromBoundary(actorId, nameof(actorId)),
            context,
            bindingToken,
            new ZLinkSessionActor(
                context,
                actorId,
                sessionRid,
                bindingToken),
            6,
            sourceRoute,
            7,
            ownerNode,
            ownerName,
            8);
        return new CanonicalBindingFixture(
            actorId,
            bindingToken,
            stream,
            CreateSeal(
                actorId,
                sessionRid,
                sourceNode,
                sourceRoute,
                6,
                7,
                8,
                actorId.EndsWith("first", StringComparison.Ordinal)
                    ? 401UL
                    : 409UL,
                actorId.EndsWith("first", StringComparison.Ordinal)
                    ? 419UL
                    : 421UL,
                ownerNode,
            ownerName));
    }

    private static CanonicalBindingFixture BindCoordinatorActor(
        ZLinkActorBoundSessionCoordinator coordinator,
        ZLinkFrameworkRuntime runtime,
        string actorId,
        string bindingToken,
        string sessionName,
        string sourceName,
        string ownerName)
    {
        var sessionRid = RoutingId.From(sessionName);
        var sourceNode = RoutingId.From(sourceName);
        var ownerNode = RoutingId.From(ownerName);
        var stream = new RecordingStream(sessionRid);
        var context = new ZLinkSessionContext(
            runtime,
            stream,
            new EmptySessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        var sourceRoute = ZLinkSessionBindingRoute.Create(
            new ActorRef(actorId, 5, "actors", sourceNode),
            "actors",
            2,
            11,
            17);
        coordinator.BindSessionActor(
            actorId,
            context,
            bindingToken,
            new ZLinkSessionActor(
                context,
                actorId,
                sessionRid,
                bindingToken),
            6,
            sourceRoute,
            7,
            ownerNode,
            ownerName,
            8);
        return new CanonicalBindingFixture(
            actorId,
            bindingToken,
            stream,
            CreateSeal(
                actorId,
                sessionRid,
                sourceNode,
                sourceRoute,
                6,
                7,
                8,
                431,
                433,
                ownerNode,
                ownerName));
    }

    private readonly record struct CanonicalBindingFixture(
        string ActorId,
        string BindingToken,
        RecordingStream Stream,
        ZLinkServiceWireCodec.SessionRelocationSealRecord Seal);

    private static Fixture CreateFixture()
    {
        const string actorId = "actor-owner-regression";
        const string bindingToken = "binding-owner-regression";
        const string meshName = "actors";
        const ulong objectGeneration = 5;
        const ulong sourceNodeGeneration = 2;
        const ulong sourceAuthority = 11;
        const ulong sourceOwnerLease = 17;
        const ulong bindingGeneration = 6;
        const ulong sessionOwnerNodeGeneration = 7;
        const ulong sessionOwnerLease = 8;
        const ulong targetNodeGeneration = 3;
        const ulong targetAuthority = 12;
        const ulong targetOwnerLease = 101;

        var registration = new ZLinkFrameworkRegistration
        {
            DefaultRequestTimeout = TimeSpan.FromSeconds(1)
        };
        var services = new ServiceCollection();
        services.AddSingleton(registration);
        var provider = services.BuildServiceProvider();
        var runtime = new ZLinkFrameworkRuntime(
            provider,
            null!,
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                provider.GetRequiredService<IServiceScopeFactory>(),
                registration));

        var sourceNode = RoutingId.From("source-node-owner-regression");
        var targetNode = RoutingId.From("target-node-owner-regression");
        var sessionOwnerNode = RoutingId.From(
            "session-owner-node-owner-regression");
        var sessionRid = RoutingId.From("session-owner-regression");
        var stream = new RecordingStream(sessionRid);
        var context = new ZLinkSessionContext(
            runtime,
            stream,
            new EmptySessionHandlerRegistry(),
            static () => ValueTask.CompletedTask,
            static _ => ValueTask.CompletedTask);
        var actor = new ActorRef(
            actorId,
            objectGeneration,
            meshName,
            sourceNode);
        var sourceRoute = ZLinkSessionBindingRoute.Create(
            actor,
            meshName,
            sourceNodeGeneration,
            sourceAuthority,
            sourceOwnerLease);
        _ = runtime.BindSessionActor(
            actorId,
            context,
            bindingToken,
            new ZLinkSessionActor(
                context,
                actorId,
                sessionRid,
                bindingToken),
            bindingGeneration,
            sourceRoute,
            sessionOwnerNodeGeneration,
            sessionOwnerNode,
            "session-owner",
            sessionOwnerLease);

        return new Fixture(
            provider,
            runtime,
            stream,
            actorId,
            bindingToken,
            meshName,
            objectGeneration,
            sourceNode,
            sourceNodeGeneration,
            sourceAuthority,
            sourceOwnerLease,
            sessionOwnerNode,
            sessionOwnerNodeGeneration,
            sessionOwnerLease,
            sessionRid,
            bindingGeneration,
            targetNode,
            targetNodeGeneration,
            targetAuthority,
            targetOwnerLease);
    }

    private sealed class Fixture(
        ServiceProvider provider,
        ZLinkFrameworkRuntime runtime,
        RecordingStream stream,
        string actorId,
        string bindingToken,
        string meshName,
        ulong objectGeneration,
        RoutingId sourceNode,
        ulong sourceNodeGeneration,
        ulong sourceAuthority,
        ulong sourceOwnerLeaseGeneration,
        RoutingId sessionOwnerNode,
        ulong sessionOwnerNodeGeneration,
        ulong sessionOwnerLeaseGeneration,
        RoutingId sessionRid,
        ulong bindingGeneration,
        RoutingId targetNode,
        ulong targetNodeGeneration,
        ulong targetAuthority,
        ulong targetOwnerLeaseGeneration) : IDisposable
    {
        public ZLinkFrameworkRuntime Runtime { get; } = runtime;
        public RecordingStream Stream { get; } = stream;
        public string ActorId { get; } = actorId;
        public string BindingToken { get; } = bindingToken;
        public RoutingId TargetNode { get; } = targetNode;
        public ulong TargetOwnerLeaseGeneration { get; } =
            targetOwnerLeaseGeneration;

        public async ValueTask<(
            ZLinkServiceWireCodec.SessionRelocationRouteRecord Route,
            ZLinkSessionRelocationAuthenticatedRoute AuthenticatedTarget)>
            SealAndCreateCommitAsync()
        {
            var relocationId = new ZLinkServiceWireCodec.RelocationWireId(
                211,
                223);
            var seal = new ZLinkServiceWireCodec.SessionRelocationSealRecord(
                relocationId,
                new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                    "source-owner",
                    sourceOwnerLeaseGeneration,
                    sourceNode,
                    sourceNodeGeneration,
                    "store-owner-regression"),
                1,
                new ZLinkServiceWireCodec.SessionActorRouteFenceRecord(
                    new ZLinkServiceWireCodec.SessionActorIdentityRecord(
                        ActorId,
                        objectGeneration),
                    sourceNode,
                    sourceNodeGeneration,
                    sourceAuthority,
                    sourceOwnerLeaseGeneration),
                new ZLinkServiceWireCodec.SessionOwnerFenceRecord(
                    sessionOwnerNode,
                    sessionOwnerNodeGeneration,
                    "session-owner",
                    sessionOwnerLeaseGeneration,
                    sessionRid,
                    bindingGeneration));
            var sealedRecord = await Runtime.SealCanonicalSessionActorRouteAsync(
                seal,
                CancellationToken.None);
            var route = new ZLinkServiceWireCodec.SessionRelocationRouteRecord(
                relocationId,
                seal.Coordinator,
                2,
                seal.Actor.Actor,
                seal.Session,
                ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Commit(
                    sourceAuthority,
                    targetAuthority,
                    TargetNode,
                    targetNodeGeneration,
                    sealedRecord.LastAcceptedSessionSequence));
            var authenticatedTarget =
                new ZLinkSessionRelocationAuthenticatedRoute(
                    TargetNode,
                    targetNodeGeneration,
                    meshName,
                    targetAuthority,
                    TargetOwnerLeaseGeneration);
            return (route, authenticatedTarget);
        }

        public void Dispose() => provider.Dispose();
    }

    private sealed class BlockingCountingLocationStore(
        IZLinkLocationStore inner) : IZLinkLocationStore
    {
        private TaskCompletionSource _readStarted = Signal();
        private TaskCompletionSource _releaseRead = Signal();
        private TaskCompletionSource _readCompleted = Signal();
        private int _authorityReadCalls;
        private int _providerReadCalls;
        private int _blockNextAuthorityRead;
        private int _readsDisabled;

        internal Task ReadStarted => _readStarted.Task;
        internal Task ReadCompleted => _readCompleted.Task;
        internal int AuthorityReadCalls =>
            Volatile.Read(ref _authorityReadCalls);

        internal void ResetAndBlockNextAuthorityRead()
        {
            _readStarted = Signal();
            _releaseRead = Signal();
            _readCompleted = Signal();
            Volatile.Write(ref _authorityReadCalls, 0);
            Volatile.Write(ref _providerReadCalls, 0);
            Volatile.Write(ref _readsDisabled, 0);
            Volatile.Write(ref _blockNextAuthorityRead, 1);
        }

        internal void ReleaseRead() => _releaseRead.TrySetResult();

        internal void DisableReads() =>
            Volatile.Write(ref _readsDisabled, 1);

        public async ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default)
        {
            if (Volatile.Read(ref _readsDisabled) != 0)
                throw new IOException("The location Store read port is disabled.");
            var providerCall = Interlocked.Increment(
                ref _providerReadCalls);
            if (key.Value.Contains("payload:", StringComparison.Ordinal))
            {
                Interlocked.Increment(ref _authorityReadCalls);
                _readStarted.TrySetResult();
                if (Interlocked.Exchange(
                        ref _blockNextAuthorityRead,
                        0) != 0)
                    await _releaseRead.Task.WaitAsync(cancellationToken);
            }
            var result = await inner.ReadAsync(key, cancellationToken);
            if (providerCall >= 3)
                _readCompleted.TrySetResult();
            return result;
        }

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default) =>
            inner.WriteAsync(request, cancellationToken);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default) =>
            inner.ScanAsync(request, cancellationToken);

        private static TaskCompletionSource Signal() =>
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class DisabledAuthorityResolver
        : ZLinkLocationStoreTestDouble
    {
        private int _readCalls;

        internal int ReadCalls => Volatile.Read(ref _readCalls);

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default)
        {
            Interlocked.Increment(ref _readCalls);
            return ValueTask.FromException<ZLinkAuthorityReadResult>(
                new InvalidOperationException(
                    "Downstream authority reauthorization is disabled."));
        }
    }

    private sealed class BlockingReadyAuthorityResolver(
        string actorId,
        ulong objectGeneration,
        ulong authorityOwnerGeneration,
        string ownerId,
        ulong ownerLeaseGeneration,
        string meshName,
        RoutingId targetNode,
        ulong targetNodeGeneration) : ZLinkLocationStoreTestDouble
    {
        private readonly TaskCompletionSource _readStarted = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly TaskCompletionSource _releaseRead = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        private int _readCalls;

        internal Task ReadStarted => _readStarted.Task;
        internal int ReadCalls => Volatile.Read(ref _readCalls);
        internal void ReleaseRead() => _releaseRead.TrySetResult();

        public override async ValueTask<ZLinkAuthorityReadResult>
            ReadAuthorityAsync(
                ZLinkAuthorityKey key,
                CancellationToken cancellationToken = default)
        {
            Interlocked.Increment(ref _readCalls);
            _readStarted.TrySetResult();
            await _releaseRead.Task.WaitAsync(cancellationToken);
            var payload = ZLinkActorAuthorityPayloadCodec.Encode(
                new ZLinkActorAuthorityPayload(
                    ZLinkActorAuthorityState.Ready,
                    "player",
                    actorId,
                    "entry:remote-proof",
                    1,
                    ZLinkSpotKind.Entry,
                    ownerId,
                    ownerLeaseGeneration,
                    meshName,
                    targetNode,
                    targetNodeGeneration));
            return new ZLinkAuthorityReadResult.Found(
                new ZLinkAuthoritySnapshot(
                    "store-remote-proof",
                    payload,
                    objectGeneration,
                    authorityOwnerGeneration,
                    ownerId,
                    checked((long)ownerLeaseGeneration),
                    new ZLinkPlacementAllocation(
                        ZLinkPlacementAllocationState.Active,
                        ZLinkPlacementObjectKind.Actor,
                        "player",
                        new ZLinkMeshNodeDescriptorKey(
                            meshName,
                            targetNode),
                        targetNodeGeneration,
                        new ZLinkCapacityVector(0, 1, null)),
                    null,
                    DateTimeOffset.UtcNow));
        }
    }

    private sealed class EmptySessionHandlerRegistry
        : IZLinkSessionHandlerRegistry
    {
        public void AddHandler<THandler>() where THandler : class
        {
        }

        public void AddHandler<THandler>(string packetName)
            where THandler : class
        {
        }

        public ValueTask<bool> TryHandleAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(false);
    }

    private sealed class RecordingStream(RoutingId routingId) : IZLinkStream
    {
        public string SessionId { get; } = routingId.ToHex();
        public RoutingId? RoutingId { get; } = routingId;
        public string? LocalAddr => null;
        public string? RemoteAddr => null;
        public List<(byte[] Payload, SendFlags Flags)> Writes { get; } = [];

        public bool Write(
            ZLinkMessage payload,
            SendFlags flags = SendFlags.None)
        {
            Writes.Add((payload.Decode<byte[]>(), flags));
            return true;
        }

        public ValueTask CloseAsync() => ValueTask.CompletedTask;
    }
}
