using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Fact]
    public async Task SameNodeJoin_PublicAndFacadeUseOneLocalPath()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node, includeJoinTarget: true);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var target = await runtime.CreateAsync<JoinTargetSpot>();
            using (var handler = ZLinkDeferredActorJoinHandlerScope.Open())
            {
                actor.Context.JoinSpot(target.Spot.SpotId, ZLinkMessage.Empty).Defer();
                handler.Complete();
            }
            var completion = await actor.JoinCompletion.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.IsType<ZLinkActorJoinCompletion.Accepted>(completion);
            Assert.Null(node.ActorJoinSubmittedParts);

            var resolved = await runtime.JoinActorAsync(
                target.Spot.SpotId,
                actor,
                ZLinkMessage.Empty,
                CancellationToken.None
            );
            Assert.IsType<ZLinkActorJoinResult.Accepted>(resolved);
            Assert.Null(node.ActorJoinSubmittedParts);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task SameNodeJoin_FacadeCommitsStoreAndMembershipBeforeLifecycleAndAccepted()
    {
        BlockingActorAuthorityStore? store = null;
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            userSpotType: typeof(ObservedLocalJoinSpot),
            locationStoreWrapper: inner => store = new BlockingActorAuthorityStore(inner)
        );
        ObservedLocalJoinSpot? spot = null;
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var target = await runtime.CreateAsync<ObservedLocalJoinSpot>();
            var activation = Assert.Single(
                runtime.GetSpotNodeRuntime("entry").Spots,
                item => item.SpotId == target.Spot.SpotId
            );
            spot = Assert.IsType<ObservedLocalJoinSpot>(activation.Spot);
            store!.ActorAuthorityKey = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.ActorId);
            var join = runtime
                .JoinActorAsync(
                    target.Spot.SpotId,
                    actor,
                    ZLinkMessage.Empty,
                    CancellationToken.None
                )
                .AsTask();
            await store.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(join.IsCompleted);
            Assert.Null(actor.Context.SpotId);
            Assert.Equal(0, activation.JoinedActorCount);
            Assert.False(spot.JoinedStarted.Task.IsCompleted);
            Assert.Null(node.ActorJoinSubmittedParts);

            store.Release.SetResult();
            await spot.JoinedStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(join.IsCompleted);
            Assert.Equal(target.Spot.SpotId, actor.Context.SpotId);
            Assert.Equal(1, activation.JoinedActorCount);
            var authority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await new ZLinkProviderLocationRepository(store).ReadAuthorityAsync(
                    ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.ActorId)
                )
            );
            Assert.True(
                ZLinkActorAuthorityPayloadCodec.TryDecode(
                    authority.Snapshot.Payload.Span,
                    out var payload
                )
            );
            Assert.Equal(target.Spot.SpotId, payload.CurrentSpotId);

            spot.JoinedRelease.SetResult();
            var result = Assert.IsType<ZLinkActorJoinResult.Accepted>(
                await join.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal(actorRef.ActorId, result.Actor.ActorId);
            Assert.Equal(actorRef.Generation, result.Actor.ObjectGeneration);
            Assert.Equal(actorRef.NodeRid, result.Actor.NodeRid);
            Assert.Equal(actorRef, runtime.GetOrCreateActorState(actor.ActorId).NativeActorRef);
            Assert.Equal(1, spot.AdmissionCount);
            Assert.Equal(1, spot.JoinedCount);
            Assert.Null(node.ActorJoinSubmittedParts);

            var repeated = Assert.IsType<ZLinkActorJoinResult.Accepted>(
                await runtime.JoinActorAsync(
                    target.Spot.SpotId,
                    actor,
                    ZLinkMessage.Empty,
                    CancellationToken.None
                )
            );
            Assert.Equal(result.Actor, repeated.Actor);
            Assert.Equal(1, spot.AdmissionCount);
            Assert.Equal(1, spot.JoinedCount);
        }
        finally
        {
            store?.Release.TrySetResult();
            spot?.JoinedRelease.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task SameNodeJoin_FacadeReportsStoreFailureWithoutPublishingMembership()
    {
        BlockingActorAuthorityStore? store = null;
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            userSpotType: typeof(ObservedLocalJoinSpot),
            locationStoreWrapper: inner => store = new BlockingActorAuthorityStore(inner)
        );
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var target = await runtime.CreateAsync<ObservedLocalJoinSpot>();
            var activation = Assert.Single(
                runtime.GetSpotNodeRuntime("entry").Spots,
                item => item.SpotId == target.Spot.SpotId
            );
            var spot = Assert.IsType<ObservedLocalJoinSpot>(activation.Spot);
            var failure = new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                "B12 Store commit failure."
            );
            store!.ActorAuthorityKey = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.ActorId);
            store.Failure = failure;
            var join = runtime
                .JoinActorAsync(
                    target.Spot.SpotId,
                    actor,
                    ZLinkMessage.Empty,
                    CancellationToken.None
                )
                .AsTask();
            await store.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            store.Release.SetResult();
            Assert.Same(failure, await Assert.ThrowsAsync<ZLinkFrameworkException>(() => join));
            Assert.Null(actor.Context.SpotId);
            Assert.Equal(0, activation.JoinedActorCount);
            Assert.Equal(0, spot.JoinedCount);
            Assert.Equal(actorRef, runtime.GetOrCreateActorState(actor.ActorId).NativeActorRef);
            Assert.Null(node.ActorJoinSubmittedParts);
        }
        finally
        {
            store?.Release.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task SameNodeJoin_FacadeKeepsCapturedDeadlineAtStoreCommit()
    {
        BlockingActorAuthorityStore? store = null;
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            userSpotType: typeof(ObservedLocalJoinSpot),
            locationStoreWrapper: inner => store = new BlockingActorAuthorityStore(inner)
        );
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var target = await runtime.CreateAsync<ObservedLocalJoinSpot>();
            var activation = Assert.Single(
                runtime.GetSpotNodeRuntime("entry").Spots,
                item => item.SpotId == target.Spot.SpotId
            );
            var spot = Assert.IsType<ObservedLocalJoinSpot>(activation.Spot);
            store!.ActorAuthorityKey = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.ActorId);
            var deadline = DateTimeOffset.UtcNow + TimeSpan.FromMilliseconds(200);
            var join = runtime
                .JoinActorAsync(
                    target.Spot.SpotId,
                    actor,
                    ZLinkMessage.Empty,
                    operationId: null,
                    CancellationToken.None,
                    deadline
                )
                .AsTask();
            await store.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                join.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, failure.Kind);
            Assert.Null(actor.Context.SpotId);
            Assert.Equal(0, activation.JoinedActorCount);
            Assert.Equal(0, spot.JoinedCount);
            Assert.Equal(actorRef, runtime.GetOrCreateActorState(actor.ActorId).NativeActorRef);
            Assert.Null(node.ActorJoinSubmittedParts);
        }
        finally
        {
            store?.Release.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task SameNodeJoin_WithoutNativeRoutingRefReportsTheLocalMissingTarget()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            var actor = new ProbeActor("b12-no-native-ref");
            var failure = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
                await runtime.JoinActorAsync(
                    "b12-missing-target",
                    actor,
                    ZLinkMessage.Empty,
                    CancellationToken.None
                )
            );
            Assert.Equal("SPOT 'b12-missing-target' is not active.", failure.Message);
            Assert.Null(node.ActorJoinSubmittedParts);
            Assert.Null(actor.Context.SpotId);
            Assert.Null(runtime.GetOrCreateActorState(actor.ActorId).NativeActorRef);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class ObservedLocalJoinSpot(IZLinkSpotContext context) : IZLinkSpot<ProbeActor>
    {
        public IZLinkSpotContext Context { get; } = context;
        public int AdmissionCount { get; private set; }
        public int JoinedCount { get; private set; }
        public TaskCompletionSource JoinedStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource JoinedRelease { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken
        )
        {
            AdmissionCount++;
            return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());
        }

        public async ValueTask OnJoinedActorAsync(
            ProbeActor actor,
            CancellationToken cancellationToken
        )
        {
            JoinedCount++;
            JoinedStarted.TrySetResult();
            await JoinedRelease.Task.WaitAsync(cancellationToken);
        }

        public ValueTask OnLeaveActorAsync(ProbeActor actor, CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }

    private sealed class BlockingActorAuthorityStore(IZLinkLocationStore inner)
        : IZLinkLocationStore
    {
        public ZLinkAuthorityKey? ActorAuthorityKey { get; set; }
        public Exception? Failure { get; set; }
        public TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        ) => inner.ReadAsync(key, cancellationToken);

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        )
        {
            if (
                ActorAuthorityKey is { } key
                && request.Mutations.Any(mutation =>
                    mutation is ZLinkStoreMutation.Put put
                    && put.Key == ZLinkProviderLocationRepository.AuthorityMetaKey(key)
                )
            )
            {
                Started.TrySetResult();
                await Release.Task.WaitAsync(cancellationToken);
                if (Failure is not null)
                    throw Failure;
            }
            return await inner.WriteAsync(request, cancellationToken);
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => inner.ScanAsync(request, cancellationToken);
    }
}
