using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    private static readonly TimeSpan AcceptedSendWait = TimeSpan.FromSeconds(5);
    private const string AcceptedSendPacket = "accepted-send";
    private const string AcceptedSendSpotId = "accepted-send-spot";
    private const string AcceptedSendSessionId = "session-rid";
    private const string AcceptedSendWarmup = "warmup";
    private const string AcceptedSendRequestPacket = "accepted-send-request";
    private const string AcceptedRelayNodeId = "accepted-relay-node";

    public enum AcceptedSendPath
    {
        NativeActorFrames,
        SpotRoute,
        SerialActorCallback,
        EntryActorFrames,
        RemoteActorFrames,
        WarmEntryActorFrames,
        InfrastructureRemoteActorFrames,
        NestedSpotRoute,
        ReservedActorReplay,
        DeferredSerialActorCallback,
        ReservedActorReply,
    }

    [Theory]
    [InlineData(false, AcceptedSendPath.NativeActorFrames)]
    [InlineData(true, AcceptedSendPath.NativeActorFrames)]
    [InlineData(false, AcceptedSendPath.SpotRoute)]
    [InlineData(true, AcceptedSendPath.SpotRoute)]
    [InlineData(false, AcceptedSendPath.SerialActorCallback)]
    [InlineData(true, AcceptedSendPath.SerialActorCallback)]
    [InlineData(false, AcceptedSendPath.EntryActorFrames)]
    [InlineData(true, AcceptedSendPath.EntryActorFrames)]
    [InlineData(false, AcceptedSendPath.RemoteActorFrames)]
    [InlineData(true, AcceptedSendPath.RemoteActorFrames)]
    [InlineData(false, AcceptedSendPath.WarmEntryActorFrames)]
    [InlineData(true, AcceptedSendPath.WarmEntryActorFrames)]
    [InlineData(false, AcceptedSendPath.InfrastructureRemoteActorFrames)]
    [InlineData(true, AcceptedSendPath.InfrastructureRemoteActorFrames)]
    [InlineData(false, AcceptedSendPath.NestedSpotRoute)]
    [InlineData(true, AcceptedSendPath.NestedSpotRoute)]
    [InlineData(false, AcceptedSendPath.ReservedActorReplay)]
    [InlineData(true, AcceptedSendPath.ReservedActorReplay)]
    [InlineData(false, AcceptedSendPath.DeferredSerialActorCallback)]
    [InlineData(true, AcceptedSendPath.DeferredSerialActorCallback)]
    [InlineData(false, AcceptedSendPath.ReservedActorReply)]
    [InlineData(true, AcceptedSendPath.ReservedActorReply)]
    public async Task Accepted_handler_send_survives_host_admission_seal(
        bool shutdown,
        AcceptedSendPath path
    )
    {
        var node = new CapturingSpotNode();
        var probe = new AcceptedSendProbe();
        probe.ForwardToActor =
            path
                is AcceptedSendPath.SerialActorCallback
                    or AcceptedSendPath.DeferredSerialActorCallback;
        probe.DeferActorSubmission = path == AcceptedSendPath.DeferredSerialActorCallback;
        using var flow = new CapturingMessageFlowObserver();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            messageFlowObserver: flow,
            messageFlowMode: ZLinkDiagnosticsLevel.Detailed,
            acceptedSendProbe: probe
        );
        using var services = new ServiceCollection().AddSingleton(probe).BuildServiceProvider();
        var nativeSpot = new CapturingSpot();
        await using var activation = new ZLinkUserSpotActivation(
            runtime,
            services.CreateAsyncScope(),
            nativeSpot,
            AcceptedSendSpotId,
            node.RoutingId,
            "entry",
            "entry",
            AcceptedSendWait,
            ZLinkUserSpotExecutionMode.PerActor
        );
        activation.AttachSpot(new AcceptedSendSpot(activation));
        ((IZLinkSpotContext)activation).Handlers.AddHandler<AcceptedSendHandler>(
            AcceptedSendPacket
        );
        ((IZLinkSpotContext)activation).Handlers.AddPacket<AcceptedRouteSendHandler>();
        ((IZLinkSpotContext)activation).Handlers.AddHandler<AcceptedSendRequestHandler>(
            AcceptedSendRequestPacket
        );
        await activation.BindDescriptorsAsync(CancellationToken.None);
        var actor = RegisterProbeActor(runtime, actorRef);
        probe.Actor = actor;
        var state = runtime.GetOrCreateActorState(actorRef.ActorId);
        probe.ActorState = state;
        activation.StageRelocatedPerActorMember(actor, state);
        activation.PublishRelocatedPerActorMember(actor, state);
        await activation._serial.ExecuteActorAsync(
            actorRef.ActorId,
            static (_, _, _) => ValueTask.CompletedTask,
            0,
            CancellationToken.None
        );
        runtime.BindActorSession(
            actorRef.ActorId,
            node.RoutingId,
            RoutingId.From(AcceptedSendSessionId),
            ZLinkActorBoundSessionBindingToken.Native(RoutingId.From(AcceptedSendSessionId)),
            objectGeneration: actorRef.Generation,
            authorityOwnerGeneration: 1,
            meshName: "entry",
            ownerLeaseGeneration: 1
        );
        var runner = new ZLinkRuntimeTaskRunner(runtime.ErrorSink, CancellationToken.None);
        await using var entryPump = new ZLinkEntrySpotDispatchPump(runtime, null, runner);
        if (path is AcceptedSendPath.EntryActorFrames or AcceptedSendPath.WarmEntryActorFrames)
            entryPump.Attach(nativeSpot);
        else
            activation.AttachNativeDispatch();
        Task? remoteDispatch = null;
        try
        {
            if (path == AcceptedSendPath.WarmEntryActorFrames)
            {
                await RaiseAcceptedActorFrameAsync(nativeSpot, actorRef, AcceptedSendWarmup);
                await probe.WarmStarted.Task.WaitAsync(AcceptedSendWait);
            }
            if (path is AcceptedSendPath.ReservedActorReplay or AcceptedSendPath.ReservedActorReply)
                remoteDispatch = DispatchReservedActorReplayAsync(
                    runtime,
                    activation,
                    probe,
                    path == AcceptedSendPath.ReservedActorReply
                );
            else if (path == AcceptedSendPath.NestedSpotRoute)
                remoteDispatch = DispatchNestedSpotRouteAsync(runtime, nativeSpot, probe);
            else if (
                path
                is AcceptedSendPath.RemoteActorFrames
                    or AcceptedSendPath.InfrastructureRemoteActorFrames
            )
                remoteDispatch = DispatchAcceptedRemoteActorFrameAsync(
                    runtime,
                    node,
                    actorRef,
                    path == AcceptedSendPath.RemoteActorFrames
                );
            else if (
                path
                is AcceptedSendPath.SpotRoute
                    or AcceptedSendPath.SerialActorCallback
                    or AcceptedSendPath.DeferredSerialActorCallback
            )
                await nativeSpot.RaiseDispatch(
                    new ZLinkBackendSpotDispatchInfo(
                        ZLinkApplicationJobOrigin.Remote,
                        ZLinkBackendSpotDispatchEvent.RouteReadable,
                        RoutedMessages: [CreateAcceptedSendRoute()]
                    )
                );
            else
                await RaiseAcceptedActorFrameAsync(nativeSpot, actorRef, "push");
            probe.WarmRelease.TrySetResult();
            var admissionReady = path
                is AcceptedSendPath.ReservedActorReplay
                    or AcceptedSendPath.ReservedActorReply
                    or AcceptedSendPath.DeferredSerialActorCallback
                ? probe.SubmissionReady.Task
                : probe.Started.Task;
            await Task.WhenAny(admissionReady, Task.Delay(AcceptedSendWait));
            Assert.True(
                admissionReady.IsCompletedSuccessfully,
                System.Text.Json.JsonSerializer.Serialize(flow.Events)
            );
            if (shutdown)
                runtime.SealApplicationAdmissionsForDrain(CancellationToken.None);
            else
            {
                Assert.NotNull(
                    await runtime.TryBeginRelocationAdmissionFenceAsync(
                        runtime.SnapshotOperationAdmissions(),
                        runtime.DrainAdmission.SnapshotActorAdmissions(),
                        new ZLinkActorHandoffDrainSnapshot(0, true),
                        CancellationToken.None
                    )
                );
            }
            Assert.True(runtime.DrainAdmission.IsSealed);
            probe.SubmissionRelease.TrySetResult();
            await Task.WhenAny(probe.Started.Task, Task.Delay(AcceptedSendWait));
            Assert.True(
                probe.Started.Task.IsCompletedSuccessfully,
                System.Text.Json.JsonSerializer.Serialize(flow.Events)
            );
            var rejected = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
                await actor.Context.BoundSession.Send(new ProbeRouteMessage("new-send")).Async()
            );
            Assert.Contains("application admission is sealed", rejected.Message);
            var requestRejected = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
                await new ZLinkActorClient(runtime)
                    .RequestToActor(actorRef.ActorId, new ProbeRouteMessage("new-request"))
                    .Async<ProbeReply>()
            );
            Assert.Contains("application admission is sealed", requestRejected.Message);
            probe.Release.TrySetResult();
            probe.WarmRelease.TrySetResult();
            probe.SubmissionRelease.TrySetResult();
            await Task.WhenAny(probe.Sent.Task, Task.Delay(AcceptedSendWait));
            Assert.True(
                probe.Sent.Task.IsCompletedSuccessfully,
                System.Text.Json.JsonSerializer.Serialize(flow.Events)
            );
            Assert.True(
                SpinWait.SpinUntil(() => node.BoundSessionReplies.Count == 1, AcceptedSendWait),
                System.Text.Json.JsonSerializer.Serialize(flow.Events)
            );
            Assert.Single(node.BoundSessionReplies);
        }
        finally
        {
            probe.Release.TrySetResult();
            probe.WarmRelease.TrySetResult();
            probe.SubmissionRelease.TrySetResult();
            if (remoteDispatch is not null)
                await remoteDispatch;
            await runner.StopAsync();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private static async Task DispatchReservedActorReplayAsync(
        ZLinkFrameworkRuntime runtime,
        ZLinkUserSpotActivation activation,
        AcceptedSendProbe probe,
        bool request
    )
    {
        using var operation = (await runtime.AdmitOperationAsync()).Enter();
        // A canonical replay runs under the target admission seal
        // (StageInboundSpotAggregateAsync), not under a source unit seal.
        var seal =
            await activation.TrySealRelocationAsync()
            ?? throw new InvalidOperationException("The target admission seal was refused.");
        const string handoffId = "accepted-replay";
        var state = probe.ActorState;
        var actorRef = state.NativeActorRef!.Value;
        state.Handoff.BeginCanonicalMaintenanceImport(handoffId, []);
        state.Handoff.MarkAuthorityCommitted(handoffId, actorRef.Generation, actorRef.Generation);
        _ = state.Handoff.PrepareCanonicalMaintenanceReplay(handoffId);
        var reservation = activation.ReserveRelocationActorReplay(seal, state.ActorId);
        try
        {
            // Replay arrives on another execution context; its queue reservation
            // already owns the accepted operation and must survive a host seal.
            using var detached = ZLinkFrameworkRuntime.EnterAcceptedOperation(null);
            probe.SubmissionReady.TrySetResult();
            await probe.SubmissionRelease.Task;
            using var body = Message.From(
                ZLinkEnvelopeCodec.EncodeJsonBytes("push", typeof(string))
            );
            var replay = new ZLinkSpotRelocationReplayAdmission(
                activation,
                seal,
                handoffId,
                reservation
            );
            using var replayScope = ZLinkSpotRelocationReplayScope.Enter(replay);
            var router = new ZLinkActorDispatchRouter(
                runtime,
                null!,
                static (_, actorState) => actorState.Context!
            );
            if (request)
            {
                var reply = await router.SubmitForReplyAsync(
                    probe.Actor,
                    state,
                    new ZlinkStreamHeader(
                        ZlinkStreamMessageKind.Request,
                        ZlinkStreamCodec.Json,
                        ZlinkStreamHeaderFlags.HasRequestSeq,
                        new ZlinkStreamRequestSeq(1),
                        AcceptedSendRequestPacket,
                        ZlinkStreamMetadata.Empty
                    ),
                    body,
                    relocationReplay: true,
                    CancellationToken.None,
                    RoutingId.From(AcceptedSendSessionId)
                );
                Assert.Equal(ZlinkStreamMessageKind.Response, reply.Kind);
            }
            else
                await router.Async(
                    probe.Actor,
                    state,
                    CreateHeader(AcceptedSendPacket),
                    body,
                    relocationReplay: true,
                    CancellationToken.None,
                    RoutingId.From(AcceptedSendSessionId)
                );
        }
        finally
        {
            reservation.Discard();
            await activation.AbortRelocationAsync(seal);
        }
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task Actor_frame_after_host_seal_is_retained_before_application_admission(
        bool shutdown,
        bool relay
    )
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        var state = runtime.GetOrCreateActorState(actorRef.ActorId);
        var sessionRid = RoutingId.From(relay ? AcceptedRelayNodeId : AcceptedSendSessionId);
        var bindingToken = ZLinkActorBoundSessionBindingToken.Native(sessionRid);
        if (relay)
            runtime.BindActorSession(
                actorRef.ActorId,
                sessionRid,
                sessionRid,
                bindingToken,
                objectGeneration: actorRef.Generation,
                authorityOwnerGeneration: 1,
                meshName: "entry",
                ownerLeaseGeneration: 1
            );
        state.Handoff.BeginCapture();
        var nativeSpot = new CapturingSpot();
        var runner = new ZLinkRuntimeTaskRunner(runtime.ErrorSink, CancellationToken.None);
        await using var pump = new ZLinkEntrySpotDispatchPump(runtime, null, runner);
        pump.Attach(nativeSpot);
        try
        {
            if (shutdown)
                runtime.SealApplicationAdmissionsForDrain(CancellationToken.None);
            else
                Assert.NotNull(
                    await runtime.TryBeginRelocationAdmissionFenceAsync(
                        runtime.SnapshotOperationAdmissions(),
                        runtime.DrainAdmission.SnapshotActorAdmissions(),
                        new ZLinkActorHandoffDrainSnapshot(0, true),
                        CancellationToken.None
                    )
                );
            Assert.True(runtime.DrainAdmission.IsSealed);
            var metadata = ZLinkActorBoundSessionHandoffMetadata.Encode(
                new ZLinkActorBoundSessionHandoffFence(
                    actorRef.ActorId,
                    actorRef.Generation,
                    sessionRid,
                    bindingToken,
                    1,
                    1
                )
            );
            if (relay)
            {
                await DispatchAcceptedRemoteActorFrameAsync(
                    runtime,
                    node,
                    actorRef,
                    admittedCaller: false,
                    "held",
                    metadata
                );
            }
            else
            {
                var parts = CreateActorRequestParts(
                        actorRef,
                        AcceptedSendPacket,
                        "held",
                        0,
                        0,
                        sourceSessionRid: AcceptedSendSessionId,
                        kind: ZlinkStreamMessageKind.Send
                    )
                    .Select(part =>
                        part with
                        {
                            SourceNodeGeneration = 1,
                            RouteContext = new ZLinkBackendActorRouteContext(
                                new MeshOperationId(1, 1),
                                0,
                                1,
                                1,
                                1,
                                IsBoundSessionRoute: true
                            ),
                            RequestSource = new ZLinkServiceWireCodec.RequestSourceFence(
                                "held-source",
                                1,
                                part.SourceNodeRid,
                                1
                            ),
                            ApplicationMetadata = metadata,
                        }
                    )
                    .ToArray();
                await nativeSpot.RaiseDispatch(
                    new ZLinkBackendSpotDispatchInfo(
                        ZLinkApplicationJobOrigin.Remote,
                        ZLinkBackendSpotDispatchEvent.ActorReadable,
                        ActorParts: parts
                    )
                );
            }
            var held = Assert.Single(state.Handoff.SnapshotFrames());
            Assert.Equal(ZLinkEnvelopeCodec.EncodeJsonBytes("held", typeof(string)), held.Body);
            Assert.Empty(node.BoundSessionReplies);
        }
        finally
        {
            state.Handoff.Reset();
            await runner.StopAsync();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    // #1877: host relocation committed its admission fence, but this Actor's
    // unit is not sealed yet (no handoff capture). The Session node relays a
    // bound-session frame to the still-current owner. Host relocation §15
    // (`Relocating`): "Spot·Actor direct: Unit seal 전까지 payload와 timer를
    // 계속 처리한다" - the frame must reach the Actor, not be dropped. The
    // shutdown seal (`Draining`) still refuses the same frame. A blocked
    // relocation rolls back (§13) while that frame's turn is still running:
    // rollback must not wait for or fail on existing-owner traffic.
    public enum UnsealedRelaySeal
    {
        RelocationFence,
        Shutdown,
        RelocationRollback,
    }

    [Theory]
    [InlineData(UnsealedRelaySeal.RelocationFence)]
    [InlineData(UnsealedRelaySeal.Shutdown)]
    [InlineData(UnsealedRelaySeal.RelocationRollback)]
    public async Task Relayed_bound_session_frame_reaches_unsealed_actor_only_during_relocation_fence(
        UnsealedRelaySeal seal
    )
    {
        var shutdown = seal == UnsealedRelaySeal.Shutdown;
        var node = new CapturingSpotNode();
        var probe = new AcceptedSendProbe();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node, acceptedSendProbe: probe);
        using var services = new ServiceCollection().AddSingleton(probe).BuildServiceProvider();
        var nativeSpot = new CapturingSpot();
        await using var activation = new ZLinkUserSpotActivation(
            runtime,
            services.CreateAsyncScope(),
            nativeSpot,
            AcceptedSendSpotId,
            node.RoutingId,
            "entry",
            "entry",
            AcceptedSendWait,
            ZLinkUserSpotExecutionMode.PerActor
        );
        activation.AttachSpot(new AcceptedSendSpot(activation));
        ((IZLinkSpotContext)activation).Handlers.AddHandler<AcceptedSendHandler>(
            AcceptedSendPacket
        );
        await activation.BindDescriptorsAsync(CancellationToken.None);
        var actor = RegisterProbeActor(runtime, actorRef);
        probe.Actor = actor;
        var state = runtime.GetOrCreateActorState(actorRef.ActorId);
        probe.ActorState = state;
        activation.StageRelocatedPerActorMember(actor, state);
        activation.PublishRelocatedPerActorMember(actor, state);
        var sessionRid = RoutingId.From(AcceptedRelayNodeId);
        var bindingToken = ZLinkActorBoundSessionBindingToken.Native(sessionRid);
        runtime.BindActorSession(
            actorRef.ActorId,
            sessionRid,
            sessionRid,
            bindingToken,
            objectGeneration: actorRef.Generation,
            authorityOwnerGeneration: 1,
            meshName: "entry",
            ownerLeaseGeneration: 1
        );
        activation.AttachNativeDispatch();
        Task? relayed = null;
        ZLinkRelocationAdmissionFence? fence = null;
        try
        {
            if (shutdown)
                runtime.SealApplicationAdmissionsForDrain(CancellationToken.None);
            else
            {
                fence = await runtime.TryBeginRelocationAdmissionFenceAsync(
                    runtime.SnapshotOperationAdmissions(),
                    runtime.DrainAdmission.SnapshotActorAdmissions(),
                    new ZLinkActorHandoffDrainSnapshot(0, true),
                    CancellationToken.None
                );
                Assert.NotNull(fence);
            }
            Assert.True(runtime.DrainAdmission.IsSealed);
            Assert.False(state.Handoff.BlocksLocalDispatch);
            var metadata = ZLinkActorBoundSessionHandoffMetadata.Encode(
                new ZLinkActorBoundSessionHandoffFence(
                    actorRef.ActorId,
                    actorRef.Generation,
                    sessionRid,
                    bindingToken,
                    1,
                    1
                )
            );
            relayed = DispatchAcceptedRemoteActorFrameAsync(
                runtime,
                node,
                actorRef,
                admittedCaller: false,
                "after-fence",
                metadata
            );
            // The handler signals Started on entry; a refused admission
            // completes the relay task with the drop instead.
            var first = await Task.WhenAny(probe.Started.Task, relayed).WaitAsync(AcceptedSendWait);
            if (shutdown)
            {
                Assert.Same(relayed, first);
                var dropped = await Assert.ThrowsAsync<ZLinkFrameworkException>(() => relayed);
                Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, dropped.Kind);
                Assert.False(probe.Started.Task.IsCompleted);
            }
            else
            {
                if (first == relayed)
                    await relayed;
                Assert.True(probe.Started.Task.IsCompletedSuccessfully);
            }
            if (seal == UnsealedRelaySeal.RelocationRollback)
            {
                // The handler is parked on probe.Release, so its operation is
                // still active while the drain executor acquires rollback.
                Assert.False(relayed.IsCompleted);
                using var rollback = runtime.TryAcquireRelocationRollbackLease(fence!.Value);
                Assert.NotNull(rollback);
                Assert.True(runtime.TryReopenRetireAdmissionsAfterRollback(fence.Value, rollback));
                Assert.False(runtime.DrainAdmission.IsSealed);
                probe.Release.TrySetResult();
                await relayed.WaitAsync(AcceptedSendWait);
            }
        }
        finally
        {
            probe.Release.TrySetResult();
            if (relayed is not null)
                await Task.WhenAny(relayed);
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    // #1877: rollback still races with new-owner admission (relocation target
    // staging takes the drain gate's actor admission). One accepted before
    // the fence keeps rollback refused until it leaves, exactly as before.
    [Fact]
    public async Task Relocation_rollback_is_refused_while_owner_opening_inbound_is_in_flight()
    {
        var (runtime, _) = await CreateStartedRuntimeAsync(new CapturingSpotNode());
        var admitted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task? staging = null;
        try
        {
            staging = Task.Run(async () =>
            {
                Assert.True(runtime.DrainAdmission.TryEnterActorAdmission(out var ownerAdmission));
                var admission = (
                    await runtime.TryAdmitInboundOperationAsync(countAsRequest: false)
                ).EnterInbound();
                Assert.True(admission.Accepted);
                using (ownerAdmission)
                using (admission.Lease)
                {
                    admitted.TrySetResult();
                    await release.Task;
                }
            });
            await admitted.Task.WaitAsync(AcceptedSendWait);
            var fence = await runtime.TryBeginRelocationAdmissionFenceAsync(
                runtime.SnapshotOperationAdmissions(),
                runtime.DrainAdmission.SnapshotActorAdmissions(),
                new ZLinkActorHandoffDrainSnapshot(0, true),
                CancellationToken.None
            );
            Assert.NotNull(fence);
            Assert.False(runtime.DrainAdmission.TryEnterActorAdmission(out _));

            Assert.Null(runtime.TryAcquireRelocationRollbackLease(fence.Value));

            release.TrySetResult();
            await staging.WaitAsync(AcceptedSendWait);
            using var rollback = runtime.TryAcquireRelocationRollbackLease(fence.Value);
            Assert.NotNull(rollback);
            Assert.True(runtime.TryReopenRetireAdmissionsAfterRollback(fence.Value, rollback));
        }
        finally
        {
            release.TrySetResult();
            if (staging is not null)
                await Task.WhenAny(staging);
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private static async Task DispatchNestedSpotRouteAsync(
        ZLinkFrameworkRuntime runtime,
        CapturingSpot spot,
        AcceptedSendProbe probe
    )
    {
        using var operation = (await runtime.AdmitOperationAsync()).Enter();
        await spot.RaiseDispatch(
            new ZLinkBackendSpotDispatchInfo(
                ZLinkApplicationJobOrigin.Remote,
                ZLinkBackendSpotDispatchEvent.RouteReadable,
                RoutedMessages: [CreateAcceptedSendRoute()]
            )
        );
        await Task.WhenAny(probe.Sent.Task, Task.Delay(AcceptedSendWait));
    }

    private static ValueTask RaiseAcceptedActorFrameAsync(
        CapturingSpot spot,
        ZLinkBackendActorRef actor,
        string value
    ) =>
        spot.RaiseDispatch(
            new ZLinkBackendSpotDispatchInfo(
                ZLinkApplicationJobOrigin.Remote,
                ZLinkBackendSpotDispatchEvent.ActorReadable,
                ActorParts: CreateActorRequestParts(
                    actor,
                    AcceptedSendPacket,
                    value,
                    0,
                    0,
                    sourceSessionRid: AcceptedSendSessionId,
                    kind: ZlinkStreamMessageKind.Send
                )
            )
        );

    private static async Task DispatchAcceptedRemoteActorFrameAsync(
        ZLinkFrameworkRuntime runtime,
        CapturingSpotNode node,
        ZLinkBackendActorRef actor,
        bool admittedCaller,
        string value = "push",
        byte[]? metadata = null
    )
    {
        var relay = RoutingId.From(AcceptedRelayNodeId);
        node.AdmittedMeshPeers.Add(
            new MeshNodePeer(
                1,
                MeshPeerSource.Discovery,
                MeshPeerState.Admitted,
                relay,
                1,
                1,
                "inproc://accepted-relay-node",
                1,
                0,
                1
            )
        );
        node.SetLocalActorAuthorityFence(actor, 1, 1);
        var admission = admittedCaller
            ? (await runtime.TryAdmitInboundOperationAsync(false)).EnterInbound()
            : new ZLinkInboundOperationAdmission(
                true,
                ZLinkFrameworkRuntime.ZLinkRuntimeOperationLease.None
            );
        Assert.True(admission.Accepted);
        using (admission.Lease)
            await runtime.DispatchRemoteActorFrameAsync(
                actor.ActorId,
                actor.Generation,
                actor.NodeRid,
                1,
                1,
                1,
                relay,
                relay,
                1,
                relay,
                1,
                metadata is null ? default : relay,
                new ZLinkServiceWireCodec.RequestSourceFence("relay-owner", 1, relay, 1),
                metadata ?? [],
                new MeshOperationId(1, 2),
                metadata is null ? (byte)1 : (byte)0,
                0,
                0,
                null,
                0,
                ZLinkStreamProtocolDefaults
                    .EncodeHeader(CreateHeader(AcceptedSendPacket))
                    .ToArray(),
                ZLinkEnvelopeCodec.EncodeJsonBytes(value, typeof(string)),
                CancellationToken.None
            );
    }

    [Fact]
    public async Task Replay_reserved_behind_the_restore_barrier_runs_before_later_owner_ingress()
    {
        const string replayOrderPacket = "replay-order";
        const string handoffId = "replay-order";
        var node = new CapturingSpotNode();
        using var flow = new CapturingMessageFlowObserver();
        var order = new AcceptedSendProbe();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            messageFlowObserver: flow,
            messageFlowMode: ZLinkDiagnosticsLevel.Detailed,
            acceptedSendProbe: order
        );
        using var services = new ServiceCollection().AddSingleton(order).BuildServiceProvider();
        var nativeSpot = new CapturingSpot();
        await using var activation = new ZLinkUserSpotActivation(
            runtime,
            services.CreateAsyncScope(),
            nativeSpot,
            AcceptedSendSpotId,
            node.RoutingId,
            "entry",
            "entry",
            AcceptedSendWait,
            ZLinkUserSpotExecutionMode.PerActor
        );
        activation.AttachSpot(new AcceptedSendSpot(activation));
        ((IZLinkSpotContext)activation).Handlers.AddHandler<ReplayOrderHandler>(replayOrderPacket);
        await activation.BindDescriptorsAsync(CancellationToken.None);
        var actor = RegisterProbeActor(runtime, actorRef);
        var state = runtime.GetOrCreateActorState(actorRef.ActorId);
        activation.StageRelocatedPerActorMember(actor, state);
        activation.PublishRelocatedPerActorMember(actor, state);
        await activation._serial.ExecuteActorAsync(
            actorRef.ActorId,
            static (_, _, _) => ValueTask.CompletedTask,
            0,
            CancellationToken.None
        );
        activation.AttachNativeDispatch();
        var router = new ZLinkActorDispatchRouter(
            runtime,
            null!,
            static (_, actorState) => actorState.Context!
        );
        Task Dispatch(string value, bool relocationReplay) =>
            router
                .Async(
                    actor,
                    state,
                    new ZlinkStreamHeader(
                        ZlinkStreamMessageKind.Send,
                        ZlinkStreamCodec.Json,
                        ZlinkStreamHeaderFlags.None,
                        null,
                        replayOrderPacket,
                        ZlinkStreamMetadata.Empty
                    ),
                    Message.From(ZLinkEnvelopeCodec.EncodeJsonBytes(value, typeof(string))),
                    relocationReplay,
                    CancellationToken.None,
                    null
                )
                .AsTask();
        Task? replay = null;
        Task? direct = null;
        try
        {
            using var operation = (await runtime.AdmitOperationAsync()).Enter();
            // A canonical replay runs under the target admission seal
            // (StageInboundSpotAggregateAsync), not under a source unit seal.
            var seal =
                await activation.TrySealRelocationAsync()
                ?? throw new InvalidOperationException("The target admission seal was refused.");
            state.Handoff.BeginCanonicalMaintenanceImport(handoffId, []);
            state.Handoff.MarkAuthorityCommitted(
                handoffId,
                actorRef.Generation,
                actorRef.Generation
            );
            _ = state.Handoff.PrepareCanonicalMaintenanceReplay(handoffId);
            var reservation = activation.ReserveRelocationActorReplay(seal, state.ActorId);

            // The target posts replay while it holds the Actor restore barrier
            // (relocation-flow §4.6), then opens ordinary admission.
            var turn = await state.ReserveHandoffRestoreBarrier().ClaimAsync();
            using (
                ZLinkSpotRelocationReplayScope.Enter(
                    new ZLinkSpotRelocationReplayAdmission(activation, seal, handoffId, reservation)
                )
            )
                replay = Dispatch("replay", relocationReplay: true);
            turn.Dispose();
            state.Handoff.ReserveCanonicalMaintenanceTrailingAndOpenAdmission(
                handoffId,
                0,
                static _ => { }
            );
            await Task.WhenAny(order.Started.Task, replay).WaitAsync(AcceptedSendWait);
            Assert.False(replay.IsFaulted, replay.Exception?.ToString());

            // Application admission opens while the replay handler awaits;
            // new-owner ingress arrives only after that, as in production.
            await activation.AbortRelocationAsync(seal);
            direct = Dispatch("direct", relocationReplay: false);
            Assert.False(direct.IsCompleted, direct.Exception?.ToString());
            order.Release.SetResult();
            await Task.WhenAll(replay, direct).WaitAsync(AcceptedSendWait);

            Assert.True(
                order.Events.SequenceEqual([
                    "replay:start",
                    "replay:end",
                    "direct:start",
                    "direct:end",
                ]),
                string.Join(",", order.Events)
                    + " "
                    + System.Text.Json.JsonSerializer.Serialize(flow.Events)
            );
        }
        finally
        {
            order.Release.TrySetResult();
            if (replay is not null)
                await Task.WhenAny(replay, Task.Delay(AcceptedSendWait));
            if (direct is not null)
                await Task.WhenAny(direct, Task.Delay(AcceptedSendWait));
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class ReplayOrderHandler(AcceptedSendProbe probe)
        : IZLinkSpotActorSendHandler<AcceptedSendSpot, ProbeActor, string>
    {
        public async ValueTask HandleAsync(
            AcceptedSendSpot spot,
            ProbeActor actor,
            IZLinkMessageContext context,
            string message,
            CancellationToken cancellationToken
        )
        {
            probe.Events.Enqueue($"{message}:start");
            if (message == "replay")
            {
                probe.Started.TrySetResult();
                await probe.Release.Task.WaitAsync(cancellationToken);
            }
            probe.Events.Enqueue($"{message}:end");
        }
    }

    private sealed class AcceptedSendProbe
    {
        public TaskCompletionSource WarmStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource WarmRelease { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public ProbeActor Actor { get; set; } = null!;
        public bool ForwardToActor { get; set; }
        public bool DeferActorSubmission { get; set; }
        public TaskCompletionSource SubmissionReady { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource SubmissionRelease { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public ZLinkActorRuntimeState ActorState { get; set; } = null!;
        public TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Sent { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public System.Collections.Concurrent.ConcurrentQueue<string> Events { get; } = new();
    }

    private static ZLinkBackendRouteReceived CreateAcceptedSendRoute()
    {
        var source = RoutingId.From("route-source-node");
        return new ZLinkBackendRouteReceived(
            ZLinkClientCallCodec.EncodeEnvelopeParts(
                ZLinkClientCallCodec.CreateEnvelope(
                    ZLinkMessageKind.Command,
                    "entry",
                    nameof(ProbeRouteMessage)
                ),
                new ProbeRouteMessage("push"),
                null
            ),
            source,
            AcceptedSendSpotId,
            null,
            null,
            targetNodeGeneration: 1,
            authorityOwnerGeneration: 1,
            ownerLeaseGeneration: 1,
            sourceNodeGeneration: 1,
            requestSource: new ZLinkServiceWireCodec.RequestSourceFence(
                "source-owner",
                1,
                source,
                1
            )
        );
    }

    private sealed class AcceptedRouteSendHandler(AcceptedSendProbe probe)
        : IZLinkSpotPacketHandler<AcceptedSendSpot, ProbeRouteMessage>
    {
        public async ValueTask HandleAsync(
            AcceptedSendSpot spot,
            ProbeRouteMessage message,
            CancellationToken cancellationToken
        )
        {
            if (!probe.ForwardToActor)
            {
                probe.Started.TrySetResult();
                await probe.Release.Task.WaitAsync(cancellationToken);
                await probe.Actor.Context.BoundSession.Send(message).Async();
                probe.Sent.TrySetResult();
                return;
            }
            using var body = Message.From(
                ZLinkEnvelopeCodec.EncodeJsonBytes(message.Value, typeof(string))
            );
            if (probe.DeferActorSubmission)
            {
                probe.SubmissionReady.TrySetResult();
                await probe.SubmissionRelease.Task.WaitAsync(cancellationToken);
            }
            await ((ZLinkUserSpotActivation)spot.Context).SubmitActorAsync(
                probe.Actor,
                probe.ActorState,
                CreateHeader(AcceptedSendPacket),
                body,
                null,
                cancellationToken,
                RoutingId.From(AcceptedSendSessionId)
            );
        }
    }

    private sealed class AcceptedSendSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
    }

    private sealed class AcceptedSendHandler(AcceptedSendProbe probe)
        : IZLinkSpotActorSendHandler<AcceptedSendSpot, ProbeActor, string>
    {
        public async ValueTask HandleAsync(
            AcceptedSendSpot spot,
            ProbeActor actor,
            IZLinkMessageContext context,
            string message,
            CancellationToken cancellationToken
        )
        {
            if (message == AcceptedSendWarmup)
            {
                probe.WarmStarted.TrySetResult();
                await probe.WarmRelease.Task.WaitAsync(cancellationToken);
                return;
            }
            probe.Started.TrySetResult();
            await probe.Release.Task.WaitAsync(cancellationToken);
            await actor.Context.BoundSession.Send(new ProbeRouteMessage(message)).Async();
            probe.Sent.TrySetResult();
        }
    }

    private sealed class AcceptedSendRequestHandler(AcceptedSendProbe probe)
        : IZLinkSpotActorRequestHandler<AcceptedSendSpot, ProbeActor, string, string>
    {
        public async ValueTask<string> HandleAsync(
            AcceptedSendSpot spot,
            ProbeActor actor,
            IZLinkMessageContext context,
            string request,
            CancellationToken cancellationToken
        )
        {
            await new AcceptedSendHandler(probe).HandleAsync(
                spot,
                actor,
                context,
                request,
                cancellationToken
            );
            return request;
        }
    }
}
