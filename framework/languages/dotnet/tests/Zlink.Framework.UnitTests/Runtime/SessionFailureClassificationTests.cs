using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Theory]
    [InlineData(ZlinkSubmitException.ErrorCode.NotConnected, "stale_target")]
    [InlineData(ZlinkSubmitException.ErrorCode.Terminated, "shutdown")]
    [InlineData(ZlinkSubmitException.ErrorCode.Backpressured, "backpressure")]
    public async Task Session_push_preserves_typed_transport_failure(
        ZlinkSubmitException.ErrorCode result,
        string reason
    )
    {
        using var flow = new CapturingMessageFlowObserver();
        var node = new CapturingSpotNode();
        var (runtime, actor) = await CreateStartedRuntimeAsync(
            node,
            messageFlowObserver: flow,
            messageFlowMode: ZLinkDiagnosticsLevel.Normal
        );
        try
        {
            var transportFailure = new ZlinkSubmitException(result);
            var stream = new FailedSessionStream(transportFailure);
            var context = new ZLinkSessionContext(
                runtime,
                stream,
                new RelaySessionHandlerRegistry(),
                static () => ValueTask.CompletedTask,
                static _ => ValueTask.CompletedTask
            );
            var bound = new ZLinkSessionActor(
                context,
                actor.ActorId,
                stream.RoutingId!.Value,
                "push-binding"
            );
            runtime.BindSessionActor(
                actor.ActorId,
                context,
                bound.BindingToken,
                bound,
                7,
                ZLinkSessionBindingRoute.Create(
                    new ActorRef(actor.ActorId, actor.Generation, "entry", actor.NodeRid),
                    "entry",
                    1,
                    1,
                    1
                ),
                11,
                sessionOwnerNodeRid: node.RoutingId,
                sessionOwnerId: "session-owner",
                sessionOwnerLeaseGeneration: 1
            );
            var identity = new ZLinkRemoteSessionPushRelay(
                actor.ActorId,
                actor.Generation,
                "entry",
                actor.NodeRid.ToHex(),
                1,
                1,
                1,
                7,
                11,
                stream.SessionId,
                ZLinkStreamFrameCodec.Encode(
                    ZLinkStreamProtocolDefaults.EncodeHeader(CreateHeader("push-terminal")).Span,
                    [1]
                )
            );
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await runtime.AdmitRemoteSessionPushOneWayAsync(
                    identity,
                    node.RoutingId,
                    CancellationToken.None
                )
            );
            Assert.Same(transportFailure, failure.InnerException);
            Assert.Equal(
                reason,
                ZLinkTraceFormat.DispatchReasonKey(ZLinkDispatchErrorReporter.ReasonFrom(failure))
            );
            Assert.Equal(1, stream.Attempts);
            Assert.Equal(
                ZLinkDispatchErrorReason.HandlerException,
                ZLinkDispatchErrorReporter.ReasonFrom(
                    new InvalidOperationException("application failure")
                )
            );
            var runner = new ZLinkRuntimeTaskRunner(
                runtime.ErrorSink,
                CancellationToken.None,
                runtime.ExecutionOwner
            );
            await using var dispatcher = Assert.IsType<ZLinkMeshNodeRouteDispatcher>(
                ZLinkMeshNodeRouteDispatcher.Create(
                    runtime.Services,
                    runtime.Registration,
                    runtime.Registration.SpotNodes["entry"],
                    runtime,
                    runner
                )
            );
            var envelope = ZLinkClientCallCodec.CreateEnvelope(
                ZLinkMessageKind.Command,
                "entry",
                ZLinkRemoteSessionPushProtocol.PacketName
            );
            await dispatcher.DispatchBatchAsync(
                [
                    new ZLinkBackendRouteReceived(
                        ZLinkEnvelopeCodec.EncodeParts(
                            envelope,
                            identity,
                            typeof(ZLinkRemoteSessionPushRelay),
                            runtime.Registration.Codecs
                        ),
                        node.RoutingId,
                        null,
                        null,
                        null
                    ),
                ],
                CancellationToken.None
            );
            await runner.StopAsync();
            var diagnostic = Assert.Single(
                flow.Events,
                item =>
                    item.PacketName == ZLinkRemoteSessionPushProtocol.PacketName
                    && item.Action == "drop"
            );
            Assert.Equal(reason, diagnostic.Reason);
            Assert.Equal(2, stream.Attempts);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Disconnect_cleanup_callback_survives_shutdown_admission_seal(bool request)
    {
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            entrySpotType: typeof(DisconnectCleanupEntrySpot)
        );
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var entry = Assert.IsType<DisconnectCleanupEntrySpot>(
                runtime.GetSpotNodeRuntime("entry").EntrySpotActivation!.EntrySpot
            );
            var sessionRid = RoutingId.From("cleanup-session");
            const string bindingToken = "cleanup-binding";
            runtime.BindActorSession(
                actorRef.ActorId,
                actorRef.NodeRid,
                sessionRid,
                bindingToken,
                bindingGeneration: 7,
                objectGeneration: actorRef.Generation,
                authorityOwnerGeneration: 1,
                meshName: "entry",
                targetNodeGeneration: 1,
                ownerLeaseGeneration: 1,
                sessionOwnerNodeGeneration: 11
            );
            runtime.SealApplicationAdmissionsForDrain(CancellationToken.None);
            bool? callbackReply = null;
            var header = new ZlinkStreamHeader(
                request ? ZlinkStreamMessageKind.Request : ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Raw,
                request ? ZlinkStreamHeaderFlags.HasRequestSeq : ZlinkStreamHeaderFlags.None,
                request ? new ZlinkStreamRequestSeq(1) : null,
                ZLinkRemoteActorJoinPackets.SessionDisconnectedPacketName,
                ZlinkStreamMetadata.Empty
            );
            using var frames = new ZLinkSpotActorFrameBatch([
                new ZLinkSpotActorFrame(
                    ZLinkApplicationJobOrigin.Remote,
                    actorRef,
                    actorRef,
                    actorRef.NodeRid,
                    sessionRid,
                    request ? 1UL : 0UL,
                    request ? ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind : 0,
                    default,
                    header,
                    Message.From(
                        ZLinkActorBoundSessionRelay.EncodeSessionDisconnected(
                            actorRef.NodeRid,
                            11,
                            7
                        )
                    ),
                    requestSource: AcceptedRequestSource(actorRef.NodeRid),
                    directReply: parts =>
                    {
                        Assert.True(
                            ZLinkStreamFrameCodec.TryDecode(
                                Assert.Single(parts).AsReadOnlySpan(),
                                out var replyHeader,
                                out var replyBody
                            )
                        );
                        Assert.Equal(
                            ZlinkStreamMessageKind.Response,
                            ZLinkStreamProtocolDefaults.DecodeHeader(replyHeader.ToArray()).Kind
                        );
                        Assert.Empty(replyBody.ToArray());
                        callbackReply = true;
                        return SubmitResult.Ok;
                    }
                ),
            ]);
            await new ZLinkActorInboundPipeline(
                runtime,
                new ZLinkEntrySpotActorInboundEndpoint(runtime)
            ).DispatchAsync(frames, CancellationToken.None);
            Assert.Equal(1, entry.DisconnectCount);
            Assert.Equal(request ? true : (bool?)null, callbackReply);
            Assert.False(runtime.TryGetActorBoundSession(actor.ActorId, out _));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Disconnect_cleanup_uses_common_shutdown_reason_for_transport_terminal()
    {
        using var flow = new CapturingMessageFlowObserver();
        var node = new CapturingSpotNode();
        var (runtime, actor) = await CreateStartedRuntimeAsync(
            node,
            messageFlowObserver: flow,
            messageFlowMode: ZLinkDiagnosticsLevel.Normal,
            includeActorFactory: false
        );
        try
        {
            var sessionRid = RoutingId.From("cleanup-failed-session");
            var context = new ZLinkSessionContext(
                runtime,
                new ZLinkManagedStream(
                    new RelayStreamSocket(),
                    sessionRid,
                    runtime.Registration.Codecs,
                    "test"
                ),
                new RelaySessionHandlerRegistry(),
                static () => ValueTask.CompletedTask,
                static _ => ValueTask.CompletedTask
            );
            var bound = new ZLinkSessionActor(
                context,
                actor.ActorId,
                sessionRid,
                "cleanup-failed-binding"
            );
            runtime.BindSessionActor(
                actor.ActorId,
                context,
                bound.BindingToken,
                bound,
                7,
                ZLinkSessionBindingRoute.Create(
                    new ActorRef(actor.ActorId, actor.Generation, "entry", actor.NodeRid),
                    "entry",
                    1,
                    1,
                    1
                ),
                11,
                sessionOwnerNodeRid: node.RoutingId,
                sessionOwnerId: "session-owner",
                sessionOwnerLeaseGeneration: 1
            );
            node.NodeSendResults.Enqueue(SubmitResult.Terminated);
            await context.ActorCoordinator.CleanupAsync(context, CancellationToken.None);
            var failure = Assert.Single(
                flow.Events,
                item => item.ActorId == actor.ActorId && item.Action == "drop"
            );
            Assert.Equal("shutdown", failure.Reason);
            Assert.NotNull(failure.ErrorType);
            Assert.NotNull(failure.ErrorMessage);
            Assert.Empty(context.Actors.Bound);
            Assert.Single(node.NodeSendAttempts);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Disconnect_cleanup_settles_when_runtime_stops_before_remote_reply()
    {
        using var flow = new CapturingMessageFlowObserver();
        var node = new CapturingSpotNode();
        var (runtime, actor) = await CreateStartedRuntimeAsync(
            node,
            messageFlowObserver: flow,
            messageFlowMode: ZLinkDiagnosticsLevel.Normal,
            includeActorFactory: false,
            defaultRequestTimeout: TimeSpan.FromSeconds(30)
        );
        try
        {
            var sessionRid = RoutingId.From("cleanup-failed-session");
            var context = new ZLinkSessionContext(
                runtime,
                new ZLinkManagedStream(
                    new RelayStreamSocket(),
                    sessionRid,
                    runtime.Registration.Codecs,
                    "test"
                ),
                new RelaySessionHandlerRegistry(),
                static () => ValueTask.CompletedTask,
                static _ => ValueTask.CompletedTask
            );
            var bound = new ZLinkSessionActor(
                context,
                actor.ActorId,
                sessionRid,
                "cleanup-failed-binding"
            );
            runtime.BindSessionActor(
                actor.ActorId,
                context,
                bound.BindingToken,
                bound,
                7,
                ZLinkSessionBindingRoute.Create(
                    new ActorRef(actor.ActorId, actor.Generation, "entry", actor.NodeRid),
                    "entry",
                    1,
                    1,
                    1
                ),
                11,
                sessionOwnerNodeRid: node.RoutingId,
                sessionOwnerId: "session-owner",
                sessionOwnerLeaseGeneration: 1
            );
            var coordinator = (ZLinkActorBoundSessionCoordinator)
                typeof(ZLinkFrameworkRuntime)
                    .GetField(
                        "_actorBoundSessionCoordinator",
                        System.Reflection.BindingFlags.Instance
                            | System.Reflection.BindingFlags.NonPublic
                    )!
                    .GetValue(runtime)!;
            var submitted = 0;
            // Admission succeeds, but the remote owner never supplies a callback reply.
            coordinator.RemoteFrameRelayAsync = (_, _, _, _, _, _, _, _, _, _, _, _, _, _) =>
            {
                submitted++;
                return ValueTask.CompletedTask;
            };
            var cleanup = coordinator
                .NotifyDisconnectedAsync(
                    coordinator.GetSessionBinding(actor.ActorId, bound.BindingToken)!,
                    node,
                    runtime.GetSpotNodeRuntime("entry").LocalRequestSource,
                    CancellationToken.None
                )
                .AsTask();
            Assert.False(cleanup.IsCompleted);
            await runtime.ForceStopAsync(CancellationToken.None);
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                cleanup.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, failure.Kind);
            Assert.Equal(1, submitted);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class FailedSessionStream(Exception failure) : IZLinkStream
    {
        public int Attempts { get; private set; }
        public string SessionId => RoutingId!.Value.ToHex();
        public RoutingId? RoutingId { get; } = Systems.Zlink.RoutingId.From("failed-session");
        public string? LocalAddr => null;
        public string? RemoteAddr => null;

        public bool Write(ZLinkMessage payload, SendFlags flags = SendFlags.None)
        {
            Attempts++;
            throw failure;
        }

        public ValueTask CloseAsync() => ValueTask.CompletedTask;
    }

    private sealed class DisconnectCleanupEntrySpot(IZLinkEntrySpotContext context)
        : IZLinkEntrySpot<ProbeActor>
    {
        public IZLinkEntrySpotContext Context { get; } = context;
        public int DisconnectCount { get; private set; }

        public void Configure() { }

        public ValueTask OnJoinedActorAsync(
            ProbeActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(ProbeActor actor, CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnDisconnectActorAsync(
            ProbeActor actor,
            CancellationToken cancellationToken
        )
        {
            DisconnectCount++;
            return ValueTask.CompletedTask;
        }
    }
}
