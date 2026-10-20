using System.Buffers.Binary;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Restarted_target_recovery_owns_prefix_and_finishes_session_route(
        bool routedJoin
    )
    {
        var node = new FifoActivationNode();
        var blobs = new SynchronizedRelocationStore();
        var probe = new DispatchProbe();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            relocationStore: blobs,
            dispatchProbe: probe
        );
        try
        {
            // No Actor instance or process-local Join admission survives the restart.
            Assert.Null(runtime.GetOrCreateActorState(actorRef.ActorId).Actor);
            var registration = runtime.Registration.SpotNodes["entry"];
            registration.ActorRelocations["probe"] = registration.ActorRelocations["probe"] with
            {
                PolicyKind = ZLinkObjectRelocationRegistration.RecreatePolicy,
            };
            var store = runtime.Registration.Locations.ResolveStore()!;
            var relocationStore = runtime.Registration.Locations.ResolveRelocationStore()!;
            var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorRef.ActorId);
            var source = Assert
                .IsType<ZLinkAuthorityReadResult.Found>(await store.ReadAuthorityAsync(key))
                .Snapshot;
            Assert.True(
                ZLinkActorAuthorityPayloadCodec.TryDecode(source.Payload.Span, out var sourceActor)
            );
            var descriptor = new ZLinkMeshNodeDescriptor(
                "entry",
                node.RoutingId,
                sourceActor.NodeGeneration,
                1,
                "inproc://entry",
                new Dictionary<string, int>(),
                "plain",
                sourceActor.OwnerId,
                checked((long)sourceActor.OwnerLeaseGeneration),
                DateTimeOffset.UtcNow
            )
            {
                EntrySpotId = sourceActor.CurrentSpotId,
            };
            var relocationId = Guid.NewGuid();
            var handoffId = relocationId.ToString("N");
            var route = new ZLinkRemoteActorBoundSessionRoute(
                RoutingId.From("session-owner-node"),
                RoutingId.From("session-1"),
                "binding-1",
                1,
                source.ObjectGeneration,
                source.AuthorityOwnerGeneration,
                "entry",
                sourceActor.NodeGeneration,
                sourceActor.OwnerLeaseGeneration,
                1,
                0,
                "session-owner",
                1
            );
            Assert.True(route.IsBound);
            var sourceFence = new ZLinkServiceWireCodec.RequestSourceFence(
                sourceActor.OwnerId,
                sourceActor.OwnerLeaseGeneration,
                sourceActor.NodeRid,
                sourceActor.NodeGeneration
            );
            var acceptedFrame = new ZLinkActorHandoffFrame(
                ZLinkApplicationJobOrigin.Remote,
                [],
                0,
                sourceActor.NodeRid.ToBytes().ToArray(),
                [],
                0,
                0,
                ZLinkStreamProtocolDefaults
                    .EncodeHeader(
                        new ZlinkStreamHeader(
                            ZlinkStreamMessageKind.Send,
                            ZlinkStreamCodec.Json,
                            ZlinkStreamHeaderFlags.None,
                            null,
                            "second",
                            ZlinkStreamMetadata.Empty
                        )
                    )
                    .ToArray(),
                System.Text.Encoding.UTF8.GetBytes("\"second\""),
                1,
                new ZLinkBackendActorRouteContext(
                    new MeshOperationId(81, 82),
                    0,
                    sourceActor.NodeGeneration,
                    source.AuthorityOwnerGeneration,
                    sourceActor.OwnerLeaseGeneration
                ),
                sourceActor.NodeGeneration,
                sourceFence
            );
            var identity = ZLinkCanonicalActorRelocationWriter.CreateInitial(
                ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
                    source,
                    sourceActor,
                    descriptor,
                    relocationId,
                    ReadOnlyMemory<byte>.Empty,
                    [new ZLinkActorAcceptedRecord(acceptedFrame, sourceFence)],
                    route,
                    maintenancePolicy: ZLinkObjectMaintenancePolicyKind.Recreate
                ),
                applicationVersion: 1
            );
            Assert.NotNull(identity.Participants.Single().SessionBinding);
            if (routedJoin)
            {
                var participant = identity.Participants.Single();
                var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
                    participant.RecoveryPayload.Span
                );
                // The recovery path uses presence to retain routed publication ownership.
                var request = new ZLinkRemoteActorJoinRequest(
                    actorRef.ActorId,
                    "probe",
                    handoffId,
                    null,
                    null,
                    "application/octet-stream",
                    "",
                    0,
                    relocationId,
                    1,
                    new byte[32],
                    "application/json",
                    [],
                    [],
                    sourceActor.CurrentSpotId,
                    node.RoutingId.ToBytes().ToArray(),
                    source.ObjectGeneration,
                    source.AuthorityOwnerGeneration,
                    TargetNodeRid: node.RoutingId.ToBytes().ToArray(),
                    TargetNodeGeneration: descriptor.LifecycleGeneration,
                    TargetSpotGeneration: descriptor.LifecycleGeneration,
                    TargetAuthorityOwnerGeneration: source.AuthorityOwnerGeneration + 1,
                    RelocationCoordinatorOwnerId: descriptor.OwnerId,
                    RelocationCoordinatorLeaseGeneration: checked(
                        (ulong)descriptor.LeaseGeneration
                    ),
                    RelocationCoordinatorNodeRid: node.RoutingId.ToBytes().ToArray(),
                    RelocationCoordinatorNodeGeneration: descriptor.LifecycleGeneration,
                    RelocationCoordinatorExpectedAuthorityStoreVersion: source.StoreVersion,
                    ActorNodeGeneration: sourceActor.NodeGeneration,
                    ExpectedOwnerLeaseGeneration: sourceActor.OwnerLeaseGeneration
                );
                var join = new ZLinkActorRelocationRecoveryRecord(
                    request,
                    descriptor.EntrySpotId!,
                    node.RoutingId.ToBytes().ToArray(),
                    descriptor.LifecycleGeneration,
                    descriptor.LifecycleGeneration,
                    source.AuthorityOwnerGeneration + 1,
                    1,
                    2,
                    null,
                    []
                );
                identity = identity with
                {
                    Participants =
                    [
                        participant with
                        {
                            RecoveryPayload = ZLinkCanonicalParticipantRecoveryCodec.Encode(
                                recovery with
                                {
                                    OperationRecovery = ZLinkActorRemoteJoinRecoveryCodec.Encode(
                                        join
                                    ),
                                }
                            ),
                        },
                    ],
                };
            }
            identity = identity with
            {
                CanonicalSourceAuthorityOwnerGeneration = source.AuthorityOwnerGeneration,
            };
            var stored = await ZLinkRelocationTreeStore.PutAsync(
                relocationStore,
                identity,
                TimeSpan.FromHours(24),
                default
            );
            var id = relocationId.ToByteArray(bigEndian: true);
            var canonical = new ZLinkCanonicalRelocationAuthorityState(
                BinaryPrimitives.ReadUInt64BigEndian(id),
                BinaryPrimitives.ReadUInt64BigEndian(id.AsSpan(8)),
                1,
                node.RoutingId.ToHex(),
                sourceActor.NodeGeneration,
                sourceActor.OwnerId,
                sourceActor.OwnerLeaseGeneration,
                node.RoutingId.ToHex(),
                descriptor.LifecycleGeneration,
                descriptor.OwnerId,
                checked((ulong)descriptor.LeaseGeneration),
                descriptor.OwnerId,
                checked((ulong)descriptor.LeaseGeneration),
                node.RoutingId.ToHex(),
                descriptor.LifecycleGeneration,
                (byte)ZLinkStandaloneActorCanonicalPhase.Activated,
                stored.Root.Reference,
                stored.Root.ChecksumCrc32c,
                1
            )
            {
                AggregateGeneration = identity.AggregateGeneration,
                CoordinatorExpectedAuthorityStoreVersion = source.StoreVersion,
            };
            var payload = ZLinkCanonicalRelocationAuthorityStateCodec.ReplaceRelocationState(
                source.Payload.Span,
                canonical,
                identity
            );
            var published = Assert
                .IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
                    await store.CompareExchangeAuthorityAsync(
                        key,
                        source.StoreVersion,
                        new ZLinkAuthorityMutation.Put(
                            payload,
                            ZLinkAuthorityGenerationTransition.NewOwner,
                            runtime.LocationLifecycle.OwnerToken,
                            source.Allocation
                        )
                    )
                )
                .Snapshot;
            var candidate = new ZLinkRelocationRecoveryCandidate(
                new ZLinkRelocationManifestReference(
                    stored.Root.Reference,
                    stored.Root.ChecksumCrc32c,
                    identity.AggregateId,
                    identity.AggregateGeneration,
                    identity.InventoryDigest
                ),
                identity,
                [new ZLinkAuthorityEntry(key, published)]
            );
            var routeSubmitted = false;
            node.OnRoute = () =>
            {
                var state = runtime.GetOrCreateActorState(actorRef.ActorId);
                // Observe the actual runtime wiring at command 44 submission.
                var authority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                    store.ReadAuthorityAsync(key).GetAwaiter().GetResult()
                );
                Assert.Equal(
                    routedJoin,
                    ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                        authority.Snapshot.Payload.Span,
                        out _
                    )
                );
                Assert.NotNull(Assert.Single(state.Handoff.SnapshotFinalReplay()).Withdraw);
                Assert.False(probe.ActorASecondStarted.Task.IsCompleted);
                Assert.False(state.Handoff.TargetActivation.IsCompleted);
                Assert.True(state.Handoff.IsCanonicalMaintenanceHandoff(handoffId));
                routeSubmitted = true;
            };
            await runtime.StandaloneActorRelocationRuntime.RecoverPublishedAsync(
                candidate,
                default
            );
            var restored = runtime.GetOrCreateActorState(actorRef.ActorId);
            Assert.NotNull(restored.Actor);
            Assert.True(
                routeSubmitted,
                $"canonical={restored.Handoff.IsCanonicalMaintenanceHandoff(handoffId)} activation={restored.Handoff.TargetActivation.Status} staged={restored.TryGetStagedRelocationSessionRoute(handoffId, out _)} committed={restored.TryGetCommittedRelocationSessionRoute(handoffId, out _)} bound={restored.TryGetBoundSession(out _)}"
            );
            Assert.True(restored.Handoff.TargetActivation.IsCompletedSuccessfully);
            Assert.True(restored.TryGetBoundSession(out _));
            await probe.ActorASecondStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally
        {
            node.OnRoute = null;
            await runtime.StopAsync(default);
        }
    }

    [Theory]
    [InlineData("request", ZlinkStreamMessageKind.Response)]
    [InlineData("missing", ZlinkStreamMessageKind.Error)]
    public async Task Owned_fifo_request_invokes_the_handler_or_missing_reply_without_reentering(
        string packet,
        ZlinkStreamMessageKind expected
    )
    {
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(new CapturingSpotNode());
        try
        {
            RegisterProbeActor(runtime, actorRef);
            var state = runtime.GetOrCreateActorState(actorRef.ActorId);
            var pipeline = new ZLinkActorInboundPipeline(
                runtime,
                new ZLinkEntrySpotActorInboundEndpoint(runtime)
            );
            var frames = ZLinkActorHandoffIngress.CaptureMovingFrames(
                ZLinkApplicationJobOrigin.Remote,
                runtime,
                CreateActorRequestParts(actorRef, packet, "owned", requestId: 0, flags: 0)
            );
            ZLinkActorReply? reply = null;
            var terminals = state.AdmitDurablePrefix([
                new ZLinkActorDurableRecord(ct => new ValueTask(
                    pipeline.DispatchOwnedFrameAsync(
                        frames,
                        (_, result, _) =>
                        {
                            reply = result;
                            return ValueTask.CompletedTask;
                        },
                        ct
                    )
                )),
            ]);
            await Task.WhenAll(terminals).WaitAsync(TimeSpan.FromSeconds(5));
            Assert.NotNull(reply);
            Assert.Equal(expected, reply.Kind);
        }
        finally
        {
            await runtime.StopAsync(default);
        }
    }

    [Theory]
    [InlineData(false, ZLinkDiagnosticsLevel.Detailed)]
    [InlineData(true, ZLinkDiagnosticsLevel.Detailed)]
    [InlineData(false, ZLinkDiagnosticsLevel.Errors)]
    [InlineData(true, ZLinkDiagnosticsLevel.Errors)]
    public async Task Owned_durable_send_without_a_handler_reports_one_dispatch_error(
        bool liveActivation,
        ZLinkDiagnosticsLevel diagnosticsLevel
    )
    {
        using var observer = new CapturingMessageFlowObserver();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            observer,
            messageFlowMode: diagnosticsLevel,
            userSpotType: typeof(EmptyUserSpot)
        );
        try
        {
            RegisterProbeActor(runtime, actorRef);
            var state = runtime.GetOrCreateActorState(actorRef.ActorId);
            if (liveActivation)
            {
                var created = await runtime.GetOrCreateAsync<EmptyUserSpot>("owned-send-spot");
                state.JoinSpot(GetSpotActivation(runtime, created.Spot.SpotId));
            }
            var pipeline = new ZLinkActorInboundPipeline(
                runtime,
                new ZLinkEntrySpotActorInboundEndpoint(runtime)
            );
            const string packet = "owned-missing-send";
            var frames = ZLinkActorHandoffIngress.CaptureMovingFrames(
                ZLinkApplicationJobOrigin.Remote,
                runtime,
                CreateActorRequestParts(
                    actorRef,
                    packet,
                    "owned",
                    requestId: 0,
                    flags: 0,
                    kind: ZlinkStreamMessageKind.Send
                )
            );
            var terminals = state.AdmitDurablePrefix([
                new ZLinkActorDurableRecord(ct => new ValueTask(
                    pipeline.DispatchOwnedFrameAsync(frames, null, ct)
                )),
            ]);
            await Task.WhenAll(terminals).WaitAsync(TimeSpan.FromSeconds(5));
            var error = Assert.Single(
                observer.Events.Where(flow => flow.PacketName == packet && flow.Outcome == "failed")
            );
            Assert.Equal("actor", error.Surface);
            Assert.Equal("send", error.MessageKind);
            Assert.Equal("no_handler", error.Reason);
            Assert.Equal("drop", error.Action);
            Assert.Equal(actorRef.ActorId, error.ActorId);
            Assert.Equal("corr-1", error.CorrelationId);
            Assert.Equal(RoutingId.From("source-session").ToHex(), error.StreamSessionId);
        }
        finally
        {
            await runtime.StopAsync(default);
        }
    }

    private sealed class FifoActivationNode
        : CapturingSpotNode,
            IZLinkBackendSessionRelocationBarrier
    {
        internal Action? OnRoute { get; set; }

        public void SetSessionRelocationBarrierTarget(
            Zlink.Framework.Runtime.Service.ISessionRelocationBarrierTarget target
        ) { }

        public void CompleteSessionRelocation(Guid relocationId) { }

        public ValueTask<ZLinkServiceWireCodec.SessionRelocationSealedRecord> SealSessionRelocationAsync(
            RoutingId sessionOwnerNodeRid,
            ZLinkServiceWireCodec.SessionRelocationSealRecord seal,
            TimeSpan timeout,
            CancellationToken cancellationToken
        ) => throw new NotSupportedException();

        public ValueTask RouteSessionRelocationAsync(
            RoutingId sessionOwnerNodeRid,
            ZLinkServiceWireCodec.SessionRelocationRouteRecord route,
            CancellationToken cancellationToken
        )
        {
            OnRoute?.Invoke();
            return ValueTask.CompletedTask;
        }
    }
}
