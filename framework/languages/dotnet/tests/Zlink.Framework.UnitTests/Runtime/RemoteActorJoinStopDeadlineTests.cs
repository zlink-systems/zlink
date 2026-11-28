using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RemoteActorJoinStopDeadlineTests
{
    [Fact]
    public async Task Target_shutdown_discards_precommit_join_without_changing_membership()
    {
        using var flowListener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener(flowPath)
            : null;
        var timeout = TimeSpan.FromSeconds(5);
        var actorId = $"target-stop-actor-{Guid.NewGuid():N}";
        var spotId = $"target-stop-spot-{Guid.NewGuid():N}";
        var trace = new RelocationBehaviorTrace { ActorId = actorId, ExpectedJoinAccepted = false };
        var transport = new CanonicalRelocationTransportProbe(trace);
        var store = new RecordingLocationStore(new ZLinkInMemoryProviderLocationStore(), trace);
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var targetStore = new ActorAuthorityCallCounter(store, key);
        var roots = new SynchronizedRelocationStore();
        trace.ReleaseJoinHandler.SetResult();
        trace.ReleaseSourceLeave.SetResult();
        trace.ReleaseTargetLifecycle.SetResult();
        transport.ReleasePrepareCall.SetResult();

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            roots,
            registerTargetSpot: false,
            canonicalTransportProbe: transport
        );
        var actor = Assert.IsType<ZLinkActorCreateResult.Created>(
            await source
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Async()
        );
        trace.SourceNodeRid = actor.Actor.NodeRid;
        trace.SourceObjectGeneration = actor.Actor.ObjectGeneration;
        await store.ObserveActorAuthorityAsync(actorId);

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            targetStore,
            roots,
            registerTargetSpot: true,
            canonicalTransportProbe: transport
        );
        trace.TargetNodeRid = target.LocalNodeRid;
        var admission = target
            .Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName)
            .ActivationAdmission;
        var spot = await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(spotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Async();
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);

        try
        {
            await source
                .Services.GetRequiredService<IZLinkActorClient>()
                .RequestToActor(actorId, new BeginBehaviorJoin(spotId, TimeSpan.FromSeconds(2)))
                .Async<BehaviorAck>();
            await transport.CutoverSendStarted.Task.WaitAsync(timeout);
            Assert.True(target.Runtime.TryGetActorState(actorId, out var staged));
            Assert.Equal(spotId, staged.LiveActivation?.SpotId);
            Assert.Equal(1, admission.Active);
            var writesBeforeStop = targetStore.Writes;

            target.Runtime.DrainAdmission.ClaimShutdown();
            using var stopDeadline = new CancellationTokenSource(timeout);
            await target.Runtime.DrainAcceptedTargetAttemptsAsync(stopDeadline.Token);

            Assert.False(target.Runtime.TryGetActorState(actorId, out _));
            Assert.Null(staged.LiveActivation);
            Assert.Equal(0, admission.Active);
            Assert.Equal(writesBeforeStop, targetStore.Writes);
            Assert.False(trace.HasTargetAuthorityMutation);
            Assert.DoesNotContain("targetMembershipJoinCallback", trace.Events);
            Assert.DoesNotContain("targetMembershipLeaveCallback", trace.Events);
        }
        finally
        {
            transport.ReleaseCutoverSend.TrySetResult();
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Remote_join_reports_unavailable_when_authority_is_unsettled_at_stop_deadline(
        bool forceStopRuntime
    )
    {
        using var flowListener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener(flowPath)
            : null;
        var timeout = TimeSpan.FromSeconds(5);
        var actorId = $"stop-deadline-actor-{Guid.NewGuid():N}";
        var spotId = $"stop-deadline-spot-{Guid.NewGuid():N}";
        var trace = new RelocationBehaviorTrace
        {
            ActorId = actorId,
            ExpectedJoinAccepted = false,
            ExpectedDeliveryNode = "source",
        };
        var transport = new CanonicalRelocationTransportProbe(trace, holdTargetReady: true);
        var store = new RecordingLocationStore(new ZLinkInMemoryProviderLocationStore(), trace)
        {
            SourceAbortWriteFailure = new IOException("The Preserve outcome is unknown."),
        };
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var sourceStore = new ActorAuthorityCallCounter(store, key);
        var roots = new SynchronizedRelocationStore();

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            sourceStore,
            roots,
            registerTargetSpot: false,
            canonicalTransportProbe: transport
        );
        var actor = Assert.IsType<ZLinkActorCreateResult.Created>(
            await source
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Async()
        );
        trace.SourceNodeRid = actor.Actor.NodeRid;
        trace.SourceObjectGeneration = actor.Actor.ObjectGeneration;
        await store.ObserveActorAuthorityAsync(actorId);
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            registerTargetSpot: true,
            canonicalTransportProbe: transport
        );
        trace.TargetNodeRid = target.LocalNodeRid;
        var spot = await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(spotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Async();
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);

        Assert.True(source.Runtime.TryGetActorState(actorId, out var sourceActor));
        var handoff = sourceActor.Handoff;
        var backlogAtCompletion = new TaskCompletionSource<ZLinkActorHandoffFrame[]>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        trace.OnPublicJoinCompleted = async _ =>
        {
            backlogAtCompletion.SetResult(
                await handoff._lane.RunAsync(() => handoff._sourceHoldFrames.ToArray())
            );
        };
        var sourceState = await source.Runtime.EnsureStartedStateAsync(CancellationToken.None);
        Task? forceStop = null;
        try
        {
            trace.ReleaseJoinHandler.SetResult();
            await source
                .Services.GetRequiredService<IZLinkActorClient>()
                .RequestToActor(actorId, new BeginBehaviorJoin(spotId, TimeSpan.FromSeconds(2)))
                .Async<BehaviorAck>();
            await transport.PrepareCallStarted.Task.WaitAsync(timeout);
            store.HeldSourceAbort = transport.RelocationId;
            transport.ReleaseTargetReadySend.SetResult();
            transport.ReleasePrepareCall.SetResult();

            // Holding cutover keeps Target from committing while the Restore deadline
            // brings Source to Preserve. The in-flight write fails at the stop deadline.
            await transport.CutoverSendStarted.Task.WaitAsync(timeout);
            await store.SourceAbortWriteStarted.Task.WaitAsync(timeout);
            Assert.Empty(trace.JoinCompletions);

            var backlogReply = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            using var backlog = CreateBacklogRequest(
                sourceActor.NativeActorRef!.Value,
                backlogReply,
                timeout
            );
            Assert.Equal(
                ZLinkActorHandoffCaptureResult.Captured,
                handoff.TryCapture(
                    backlog,
                    source.Runtime,
                    static (runtime, frame) =>
                        ZLinkActorInboundPipeline.EnsureRelocationReplyRoute(runtime, frame)
                )
            );
            var queued = Assert.Single(
                await handoff._lane.RunAsync(() => handoff._sourceHoldFrames.ToArray())
            );

            // Count only reads and writes of this Actor's authority metadata key.
            var callsBeforeStop = sourceStore.Calls;
            using var releasePreserve = sourceState.ForceStopTokenSource.Token.Register(() =>
                store.ReleaseSourceAbortWrite.TrySetResult()
            );
            if (forceStopRuntime)
                forceStop = source.Runtime.ForceStopAsync(CancellationToken.None).AsTask();
            else
                sourceState.ForceStopTokenSource.Cancel();
            await trace.WaitAsync("publicJoinCompleted");
            Assert.Same(queued, Assert.Single(await backlogAtCompletion.Task.WaitAsync(timeout)));
            if (forceStop is not null)
                await forceStop.WaitAsync(timeout);

            Assert.Empty(trace.DeliveredMarkers);
            Assert.False(backlogReply.Task.IsCompleted);
            var completion = Assert.Single(trace.JoinCompletions);
            Assert.Equal("source", completion.Node);
            Assert.Equal(
                ZLinkFrameworkErrorKind.Unavailable,
                Assert.IsType<ZLinkActorJoinCompletion.Failed>(completion.Completion).Kind
            );
            Assert.Equal(callsBeforeStop, sourceStore.Calls);
            var unsettled = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await new ZLinkProviderLocationRepository(store).ReadAuthorityAsync(key)
            );
            Assert.Equal(store.CapturedSourceAbort!.StoreVersion, unsettled.Snapshot.StoreVersion);
        }
        finally
        {
            transport.ReleasePrepareCall.TrySetResult();
            transport.ReleaseTargetReadySend.TrySetResult();
            transport.ReleaseCutoverSend.TrySetResult();
            store.ReleaseSourceAbortWrite.TrySetResult();
            trace.ReleaseSourceLeave.TrySetResult();
            trace.ReleaseTargetLifecycle.TrySetResult();
            if (forceStop is not null)
                await forceStop.WaitAsync(timeout);
        }
    }

    [Fact]
    public async Task Source_stop_deadline_failure_uses_the_committed_target_join_operation_id()
    {
        using var flowListener = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        )
            is { } flowPath
            ? new TestHostMessageFlowListener(flowPath)
            : null;
        var timeout = TimeSpan.FromSeconds(5);
        var actorId = $"committed-stop-actor-{Guid.NewGuid():N}";
        var spotId = $"committed-stop-spot-{Guid.NewGuid():N}";
        var trace = new RelocationBehaviorTrace { ActorId = actorId, ExpectedJoinAccepted = null };
        var transport = new CanonicalRelocationTransportProbe(trace);
        var store = new RecordingLocationStore(new ZLinkInMemoryProviderLocationStore(), trace);
        var sourceStore = new SourceSettlementReadGate(store, actorId);
        var roots = new SynchronizedRelocationStore();
        var sourceCompleted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        trace.OnPublicJoinCompleted = _ =>
        {
            if (trace.JoinCompletions.Any(completion => completion.Node == "source"))
                sourceCompleted.TrySetResult();
            return ValueTask.CompletedTask;
        };
        trace.ReleaseJoinHandler.SetResult();
        trace.ReleaseSourceLeave.SetResult();
        trace.ReleaseTargetLifecycle.SetResult();
        transport.ReleasePrepareCall.SetResult();

        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            sourceStore,
            roots,
            registerTargetSpot: false,
            canonicalTransportProbe: transport
        );
        var actor = Assert.IsType<ZLinkActorCreateResult.Created>(
            await source
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Async()
        );
        trace.SourceNodeRid = actor.Actor.NodeRid;
        trace.SourceObjectGeneration = actor.Actor.ObjectGeneration;
        await store.ObserveActorAuthorityAsync(actorId);

        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            roots,
            registerTargetSpot: true,
            canonicalTransportProbe: transport
        );
        trace.TargetNodeRid = target.LocalNodeRid;
        var spot = await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(spotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Async();
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);

        var sourceState = await source.Runtime.EnsureStartedStateAsync(CancellationToken.None);
        try
        {
            await source
                .Services.GetRequiredService<IZLinkActorClient>()
                .RequestToActor(actorId, new BeginBehaviorJoin(spotId, TimeSpan.FromSeconds(2)))
                .Async<BehaviorAck>();
            await transport.CutoverSendStarted.Task.WaitAsync(timeout);
            sourceStore.HoldReads(
                sourceState.ForceStopTokenSource.Token,
                sourceState.TaskRunner.ShutdownToken
            );
            transport.ReleaseCutoverSend.SetResult();

            await trace.WaitAsync("publicJoinCompleted");
            var targetCompletion = Assert.Single(trace.JoinCompletions);
            Assert.Equal("target", targetCompletion.Node);
            var accepted = Assert.IsType<ZLinkActorJoinCompletion.Accepted>(
                targetCompletion.Completion
            );
            Assert.True(trace.HasTargetAuthorityMutation);
            Assert.Contains("targetLifecycleCompleted", trace.Events);

            await sourceStore.StopDeadlineReadStarted.Task.WaitAsync(timeout);
            sourceState.ForceStopTokenSource.Cancel();
            await sourceCompleted.Task.WaitAsync(timeout);

            var sourceCompletion = Assert.Single(
                trace.JoinCompletions,
                completion => completion.Node == "source"
            );
            var failed = Assert.IsType<ZLinkActorJoinCompletion.Failed>(
                sourceCompletion.Completion
            );
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failed.Kind);
            Assert.Equal(accepted.OperationId, failed.OperationId);
        }
        finally
        {
            transport.ReleaseCutoverSend.TrySetResult();
            sourceStore.ReleaseReads.TrySetResult();
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Shutdown_reports_force_stopped_when_the_stop_deadline_expires(
        bool expiresBeforeDrain
    )
    {
        await using var host = await RelocationBehaviorHost.StartAsync(
            "source",
            new RelocationBehaviorTrace(),
            new ZLinkInMemoryProviderLocationStore(),
            new SynchronizedRelocationStore(),
            registerTargetSpot: false
        );
        Assert.True(host.Runtime.DrainAdmission.TryEnterActorAdmission(out var admission));
        using (admission)
        {
            // The admitted work stays pending whether the deadline expires before or during drain.
            var deadline = expiresBeforeDrain
                ? TimeSpan.FromTicks(1)
                : TimeSpan.FromMilliseconds(100);
            var result = await host
                .Services.GetRequiredService<IZLinkFrameworkRuntime>()
                .ShutdownAsync(deadline)
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5));

            Assert.Equal(ZLinkFrameworkTerminationOutcome.ForceStopped, result.Outcome);
            Assert.Equal(ZLinkFrameworkTerminationReason.DeadlineExceeded, result.Reason);
        }
    }

    private sealed class SourceSettlementReadGate(IZLinkLocationStore inner, string actorId)
        : IZLinkLocationStore
    {
        private readonly ZLinkStoreKey _authorityKey =
            ZLinkProviderLocationRepository.AuthorityMetaKey(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId)
            );
        private readonly TaskCompletionSource<(
            CancellationToken StopDeadline,
            CancellationToken SourceLeave
        )> _holdReads = new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal TaskCompletionSource StopDeadlineReadStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource ReleaseReads { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal void HoldReads(CancellationToken stopDeadline, CancellationToken sourceLeave) =>
            _holdReads.SetResult((stopDeadline, sourceLeave));

        public async ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        )
        {
            if (
                key == _authorityKey
                && _holdReads.Task.IsCompletedSuccessfully
                && cancellationToken != _holdReads.Task.Result.SourceLeave
            )
            {
                // Only the settlement read after the Restore deadline uses this token.
                if (cancellationToken == _holdReads.Task.Result.StopDeadline)
                    StopDeadlineReadStarted.TrySetResult();
                await ReleaseReads.Task.WaitAsync(cancellationToken);
            }
            return await inner.ReadAsync(key, cancellationToken);
        }

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        ) => inner.WriteAsync(request, cancellationToken);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => inner.ScanAsync(request, cancellationToken);
    }

    private static ZLinkSpotActorFrame CreateBacklogRequest(
        ZLinkBackendActorRef actor,
        TaskCompletionSource reply,
        TimeSpan timeout
    )
    {
        const ulong requestId = 1;
        const ulong nodeGeneration = 1;
        const uint flags = ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind;
        return new ZLinkSpotActorFrame(
            ZLinkApplicationJobOrigin.Remote,
            actor,
            actor,
            actor.NodeRid,
            default,
            requestId,
            flags,
            new ZLinkBackendActorRouteContext(
                new MeshOperationId(1, requestId),
                MessageFollowHopCount: 0,
                TargetNodeGeneration: nodeGeneration,
                AuthorityOwnerGeneration: 1,
                OwnerLeaseGeneration: 1,
                ReplyRequestId: requestId,
                ReplyFlags: flags,
                DeadlineUnixMs: checked(
                    (ulong)(DateTimeOffset.UtcNow + timeout).ToUnixTimeMilliseconds()
                )
            ),
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Request,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                new ZlinkStreamRequestSeq(requestId),
                nameof(BehaviorWork),
                ZlinkStreamMetadata.Empty
            ),
            Message.From("""{"marker":"prefix"}"""),
            nodeGeneration,
            new ZLinkServiceWireCodec.RequestSourceFence(
                "source",
                1,
                actor.NodeRid,
                nodeGeneration
            ),
            directReply: _ =>
            {
                reply.SetResult();
                return SubmitResult.Ok;
            }
        );
    }

    private sealed class ActorAuthorityCallCounter(IZLinkLocationStore inner, ZLinkAuthorityKey key)
        : IZLinkLocationStore
    {
        private readonly ZLinkStoreKey _authorityKey =
            ZLinkProviderLocationRepository.AuthorityMetaKey(key);
        private int _calls;
        private int _writes;

        internal int Calls => Volatile.Read(ref _calls);
        internal int Writes => Volatile.Read(ref _writes);

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        )
        {
            if (key == _authorityKey)
                Interlocked.Increment(ref _calls);
            return inner.ReadAsync(key, cancellationToken);
        }

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        )
        {
            if (
                request.Mutations.Any(mutation =>
                    mutation is ZLinkStoreMutation.Put put && put.Key == _authorityKey
                    || mutation is ZLinkStoreMutation.Delete delete && delete.Key == _authorityKey
                )
            )
            {
                Interlocked.Increment(ref _calls);
                Interlocked.Increment(ref _writes);
            }
            return inner.WriteAsync(request, cancellationToken);
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        ) => inner.ScanAsync(request, cancellationToken);
    }
}
