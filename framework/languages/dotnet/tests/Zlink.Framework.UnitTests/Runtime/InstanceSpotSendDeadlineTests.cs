using System.Collections.Concurrent;
using System.Security.Cryptography;
using Systems.Zlink.Framework.Runtime.Protocol;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Fact]
    public async Task InfrastructureRequest_DoesNotAdmitUserRequestWaitingForAuthority()
    {
        const string spotId = "held-user-authority";
        HeldInstanceResolveStore? store = null;
        var node = new CapturingSpotNode
        {
            NodeRequestHandler = (_, _) =>
                ValueTask.FromResult(
                    new ZLinkBackendRouteReceived(
                        [Message.From("authority reply")],
                        null,
                        null,
                        null,
                        null
                    )
                ),
        };
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            locationStoreWrapper: inner => store = new(inner, spotId)
        );
        using var cancellation = new CancellationTokenSource();
        try
        {
            var request = new ZLinkSpotClient(runtime)
                .RequestToSpot(spotId, new ProbeRouteMessage("request"))
                .InstanceSpot("Tests.InstanceSpot")
                .InMesh("entry")
                .Async<ProbeReply>(cancellation.Token);
            await store!.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            using var payload = Message.From("authority lookup");
            using var infrastructure = await runtime
                .GetSpotNodeRuntime("entry")
                .RequestToNodeAsync(
                    node.RoutingId,
                    [payload],
                    TimeSpan.FromSeconds(5),
                    CancellationToken.None
                );
            Assert.Equal(1, node.NodeRequestAttempts);
            Assert.False(request.Admitted.IsCompleted);
            Assert.False(request.Reply.IsCompleted);
            cancellation.Cancel();
            var failure = await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                request.AsTask()
            );
            Assert.Same(
                failure,
                await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                    request.Admitted.AsTask()
                )
            );
        }
        finally
        {
            cancellation.Cancel();
            store!.Release.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task LocalInstanceActivation_PublicRequest_AdmitsBeforeReply()
    {
        var probe = new ColdSendProbe();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            instanceSpotType: typeof(AdmissionInstanceSpot),
            instanceDispatchProbe: probe,
            relocationStore: new InMemoryRelocationStore()
        );
        try
        {
            var request = new ZLinkSpotClient(runtime)
                .RequestToSpot("admission-cold", new ProbeRouteMessage("activate"))
                .InstanceSpot("Tests.InstanceSpot")
                .InMesh("entry")
                .Async<ProbeReply>();
            await request.Admitted.AsTask().WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(request.Reply.IsCompleted);
            Assert.Equal("activate", await probe.Message.Task.WaitAsync(TimeSpan.FromSeconds(5)));
            Assert.False(request.Reply.IsCompleted);
            probe.Release.SetResult();
            Assert.Equal("activate", (await request).Value);
        }
        finally
        {
            probe.Release.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task RemoteInstanceActivation_PublicRequest_AdmitsBeforeReply()
    {
        var node = new PendingInstanceRequestNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            relocationStore: new InMemoryRelocationStore(),
            meshResolverWrapper: inner => new RemoteInstanceResolver(inner)
        );
        try
        {
            await PublishCandidateAsync(
                RequireLocationStore(runtime),
                "remote-instance-owner",
                RoutingId.From("remote-instance-node"),
                TimeSpan.FromMinutes(1)
            );
            var request = new ZLinkSpotClient(runtime)
                .RequestToSpot("admission-remote-cold", new ProbeRouteMessage("activate"))
                .InstanceSpot("Tests.InstanceSpot")
                .InMesh("entry")
                .Async<ProbeReply>();
            await node.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await request.Admitted.AsTask().WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(request.Reply.IsCompleted);
            node.Release.SetResult();
            Assert.Equal("activated", (await request).Value);
        }
        finally
        {
            node.Release.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class PendingInstanceRequestNode : CapturingSpotNode, IZLinkBackendSpotNode
    {
        internal TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async ValueTask<IReadOnlyList<Message>> ActivateInstanceSpotAsync(
            InstanceSpotActivationTarget target,
            string sourceSpotId,
            IReadOnlyList<Message> parts,
            bool request,
            ulong deadlineUnixMs,
            TimeSpan timeout,
            ReadOnlyMemory<byte> metadata,
            CancellationToken cancellationToken,
            ZLinkRequestAdmission? requestAdmission = null
        )
        {
            Assert.True(request);
            Assert.NotNull(requestAdmission);
            Assert.Equal(RoutingId.From("remote-instance-node"), target.TargetNodeRid);
            requestAdmission.Complete();
            Entered.SetResult();
            await Release.Task.WaitAsync(cancellationToken);
            return ZLinkEnvelopeCodec.EncodeParts(
                ZLinkEnvelopeCodec.DecodeHeader(parts) with
                {
                    Kind = ZLinkMessageKind.Response,
                    MessageName = string.Empty,
                },
                new ProbeReply("activated"),
                typeof(ProbeReply),
                codecs: null
            );
        }
    }

    private sealed class AdmissionInstanceSpot(IZLinkInstanceSpotContext context)
        : IZLinkInstanceSpot
    {
        public IZLinkInstanceSpotContext Context { get; } = context;

        public void Configure() => Context.Handlers.AddPacket<AdmissionInstanceHandler>();
    }

    private sealed class AdmissionInstanceHandler(ColdSendProbe probe)
        : IZLinkSpotRequestHandler<AdmissionInstanceSpot, ProbeRouteMessage, ProbeReply>
    {
        public async ValueTask<ProbeReply> HandleAsync(
            AdmissionInstanceSpot spot,
            ProbeRouteMessage request,
            CancellationToken cancellationToken
        )
        {
            probe.Message.SetResult(request.Value);
            await probe.Release.Task.WaitAsync(cancellationToken);
            return new ProbeReply(request.Value);
        }
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task ColdActivationDispatchRestoresCanonicalRootAndReplyRoute(
        bool recovered,
        bool failHandler
    )
    {
        var node = new CapturingSpotNode();
        var blobs = new InMemoryRelocationStore();
        var repository = new ZLinkProviderRelocationRepository(blobs);
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            instanceSpotType: typeof(RecoveryColdSpot),
            relocationStore: blobs
        );
        try
        {
            var store = RequireLocationStore(runtime);
            var descriptor = Assert.Single(
                await store.ListAllMeshNodesAsync("entry", CancellationToken.None),
                candidate => candidate.Rid == node.RoutingId
            );
            var owner = new ZLinkLocationOwnerToken(descriptor.OwnerId, descriptor.LeaseGeneration);
            var operation = new InstanceSpotActivationOperation(
                new InstanceSpotActivationTarget(
                    "entry",
                    node.RoutingId,
                    descriptor.LifecycleGeneration,
                    "recovered-cold",
                    "Tests.InstanceSpot",
                    descriptor.DescriptorRevision.ToString(
                        System.Globalization.CultureInfo.InvariantCulture
                    )
                ),
                node.RoutingId,
                descriptor.LifecycleGeneration,
                "source-spot",
                new MeshOperationId(101, 103),
                true,
                700,
                checked((ulong)DateTimeOffset.UtcNow.AddSeconds(5).ToUnixTimeMilliseconds())
            );
            var message = new ProbeRouteMessage(
                (failHandler ? "fail:" : "first:") + Guid.NewGuid().ToString("N")
            );
            var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(
                ZLinkClientCallCodec.CreateEnvelope(
                    ZLinkMessageKind.Request,
                    "entry",
                    ZLinkMessageNameResolver.ResolveFromMessage(message)
                ),
                message,
                runtime.Registration.Codecs
            );
            var payload = parts
                .Select(static part => (ReadOnlyMemory<byte>)part.ToArray())
                .ToArray();
            ZLinkMessageParts.DisposeAll(parts);
            var target = new ZLinkInstanceSpotActivationTarget(
                store,
                repository,
                runtime.GetSpotNodeRuntime("entry").Catalog,
                node,
                runtime.Registration.SpotNodes["entry"],
                () => owner
            );
            InstanceSpotActivationTerminal? terminal = null;
            if (recovered)
            {
                var root = ZLinkServiceWireCodec.EncodeInstanceSpotActivationRecovery(
                    operation,
                    null,
                    payload
                );
                var stored = await repository.PutRelocationAsync(root, TimeSpan.FromMinutes(1));
                var creating = new ZLinkInstanceSpotAuthorityPayload(
                    ZLinkInstanceSpotAuthorityState.Creating,
                    operation.Target.TargetSpotId,
                    operation.Target.StableType,
                    "entry",
                    node.RoutingId,
                    descriptor.LifecycleGeneration,
                    owner.OwnerId,
                    checked((ulong)owner.LeaseGeneration),
                    null
                );
                Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                    await store.ReserveAsync(
                        new ZLinkObjectReservationRequest(
                            ZLinkPlacementObjectKind.InstanceSpot,
                            ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(
                                operation.Target.TargetSpotId
                            ),
                            operation.Target.StableType,
                            stored.Reference,
                            SHA256.HashData(root),
                            root.Length,
                            new ZLinkMeshNodeDescriptorKey("entry", node.RoutingId),
                            descriptor.LifecycleGeneration,
                            owner,
                            ZLinkInstanceSpotAuthorityPayloadCodec.Encode(creating),
                            new ZLinkCapacityVector(
                                0,
                                1,
                                new ZLinkSpotTypeCapacityDelta(
                                    ZLinkPlacementObjectKind.InstanceSpot,
                                    operation.Target.StableType,
                                    1
                                )
                            )
                        )
                    )
                );
                await target.RecoverAsync(CancellationToken.None);
                Assert.Equal(
                    1,
                    RecoveryColdHandler.Received.Count(value => value == message.Value)
                );
                var authority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                    await store.ReadAuthorityAsync(
                        ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(
                            operation.Target.TargetSpotId
                        )
                    )
                );
                Assert.True(
                    ZLinkInstanceSpotAuthorityPayloadCodec.TryDecode(
                        authority.Snapshot.Payload.Span,
                        out var cleared
                    )
                );
                Assert.Null(cleared.ActivationRecovery);
                Assert.IsType<ZLinkRelocationReadResult.Missing>(
                    await repository.GetRelocationAsync(stored.Reference)
                );
                return;
            }
            else
                terminal = await target.ActivateAsync(
                    operation,
                    null,
                    payload,
                    CancellationToken.None
                );
            Assert.Equal(RequestResult.Ok, terminal!.Result);
            if (failHandler)
            {
                var error = Assert.Throws<ZLinkFrameworkException>(() =>
                    ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<ProbeReply>(
                        terminal.ReplyParts.Select(Message.From).ToArray(),
                        "empty",
                        "failed",
                        runtime.Registration.Codecs
                    )
                );
                Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
            }
            else
                Assert.Equal(
                    message.Value + "-reply",
                    ZLinkClientCallCodec
                        .DecodeEnvelopeReplyAndDispose<ProbeReply>(
                            terminal.ReplyParts.Select(Message.From).ToArray(),
                            "empty",
                            "failed",
                            runtime.Registration.Codecs
                        )
                        .Value
                );
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ReserveLoserReturnsUnavailableWithoutDispatchingItsPayload()
    {
        var node = new CapturingSpotNode();
        var blobs = new InMemoryRelocationStore();
        var repository = new ZLinkProviderRelocationRepository(blobs);
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            instanceSpotType: typeof(RecoveryColdSpot),
            relocationStore: blobs
        );
        try
        {
            var store = RequireLocationStore(runtime);
            var descriptor = Assert.Single(
                await store.ListAllMeshNodesAsync("entry", CancellationToken.None),
                row => row.Rid == node.RoutingId
            );
            var operation = new InstanceSpotActivationOperation(
                new InstanceSpotActivationTarget(
                    "entry",
                    node.RoutingId,
                    descriptor.LifecycleGeneration,
                    "reserve-loser",
                    "Tests.InstanceSpot",
                    descriptor.DescriptorRevision.ToString(
                        System.Globalization.CultureInfo.InvariantCulture
                    )
                ),
                node.RoutingId,
                descriptor.LifecycleGeneration,
                "source",
                new MeshOperationId(201, 203),
                true,
                900,
                checked((ulong)DateTimeOffset.UtcNow.AddSeconds(5).ToUnixTimeMilliseconds())
            );
            var target = new ZLinkInstanceSpotActivationTarget(
                store,
                repository,
                runtime.GetSpotNodeRuntime("entry").Catalog,
                node,
                runtime.Registration.SpotNodes["entry"],
                () => new ZLinkLocationOwnerToken(descriptor.OwnerId, descriptor.LeaseGeneration)
            );
            var message = new ProbeRouteMessage(Guid.NewGuid().ToString("N"));
            var encoded = ZLinkClientCallCodec.EncodeEnvelopeParts(
                ZLinkClientCallCodec.CreateEnvelope(
                    ZLinkMessageKind.Request,
                    "entry",
                    ZLinkMessageNameResolver.ResolveFromMessage(message)
                ),
                message,
                runtime.Registration.Codecs
            );
            var payload = encoded.Select(part => (ReadOnlyMemory<byte>)part.ToArray()).ToArray();
            ZLinkMessageParts.DisposeAll(encoded);
            Assert.Equal(
                RequestResult.Ok,
                (
                    await target.ActivateAsync(operation, null, payload, CancellationToken.None)
                ).Result
            );
            Assert.Equal(1, RecoveryColdHandler.Received.Count(value => value == message.Value));
            var loser = operation with
            {
                OperationId = new MeshOperationId(211, 213),
                ReplyRouteId = 901,
            };
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                target.ActivateAsync(loser, null, payload, CancellationToken.None).AsTask()
            );
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
            Assert.Equal(1, RecoveryColdHandler.Received.Count(value => value == message.Value));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class RecoveryColdSpot(IZLinkInstanceSpotContext context) : IZLinkInstanceSpot
    {
        public IZLinkInstanceSpotContext Context { get; } = context;

        public void Configure() => Context.Handlers.AddPacket<RecoveryColdHandler>();
    }

    private sealed class RecoveryColdHandler
        : IZLinkSpotRequestHandler<RecoveryColdSpot, ProbeRouteMessage, ProbeReply>
    {
        internal static readonly ConcurrentQueue<string> Received = new();

        public ValueTask<ProbeReply> HandleAsync(
            RecoveryColdSpot spot,
            ProbeRouteMessage request,
            CancellationToken cancellationToken
        )
        {
            Received.Enqueue(request.Value);
            return request.Value.StartsWith("fail:", StringComparison.Ordinal)
                ? throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NotFound,
                    "handler failure"
                )
                : ValueTask.FromResult(new ProbeReply(request.Value + "-reply"));
        }
    }

    [Theory]
    [InlineData("absent", 1, 100, false, ZLinkFrameworkErrorKind.NotFound)]
    [InlineData("", 2, 100, false, ZLinkFrameworkErrorKind.InvalidOperation)]
    [InlineData("remote", 1, 0, false, ZLinkFrameworkErrorKind.Unavailable)]
    [InlineData("remote", 1, 100, true, ZLinkFrameworkErrorKind.Unavailable)]
    [InlineData("", 0, 100, false, ZLinkFrameworkErrorKind.NotFound)]
    public async Task ColdActivationClassifiesServingTypesBeforeCapacity(
        string requestedType,
        int typeCount,
        int weight,
        bool full,
        ZLinkFrameworkErrorKind expected
    )
    {
        var (runtime, _) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            relocationStore: new InMemoryRelocationStore(),
            meshResolverWrapper: inner => new ColdTypeResolver(inner, typeCount, weight, full)
        );
        try
        {
            var call = new ZLinkSpotClient(runtime).SendToSpot(
                "missing-classification",
                new ProbeRouteMessage("first")
            );
            call =
                requestedType.Length == 0 ? call.InstanceSpot() : call.InstanceSpot(requestedType);
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                call.Async().AsTask()
            );
            Assert.Equal(expected, error.Kind);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ColdActivationUsesRemoteServingTypeWithoutSourceFactory(bool explicitType)
    {
        var node = new PendingInstanceSendNode();
        node.Release.TrySetResult();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            relocationStore: new InMemoryRelocationStore(),
            meshResolverWrapper: inner => new ColdTypeResolver(inner, 1, 100)
        );
        try
        {
            runtime.Registration.SpotNodes["entry"].InstanceSpotFactories.Clear();
            var call = new ZLinkSpotClient(runtime).SendToSpot(
                "remote-only",
                new ProbeRouteMessage("first")
            );
            call = explicitType ? call.InstanceSpot("remote") : call.InstanceSpot();
            await call.Async();
            Assert.Equal(1, node.Submissions);
            Assert.Equal("remote", node.StableType);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class ColdTypeResolver(
        IZLinkMeshNodeLocationResolver inner,
        int typeCount,
        int weight,
        bool full = false
    ) : IZLinkMeshNodeLocationResolver
    {
        public async ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
            string meshName,
            CancellationToken cancellationToken = default
        )
        {
            var descriptor = Assert.Single(
                await inner.ListLiveMeshNodesAsync(meshName, cancellationToken),
                candidate => candidate.Rid == RoutingId.From("entry-node")
            );
            return
            [
                descriptor with
                {
                    Rid = RoutingId.From("remote-instance-node"),
                    PlacementWeight = weight,
                    Capacity = descriptor.Capacity with
                    {
                        Spots = new ZLinkPopulationCapacity(full ? 1 : 0, 0, full ? 1 : 0),
                        SpotTypes =
                        [
                            new ZLinkSpotTypeCapacity(
                                ZLinkPlacementObjectKind.InstanceSpot,
                                "remote",
                                0,
                                0,
                                0
                            ),
                        ],
                    },
                    ObjectCapabilities = Enumerable
                        .Range(0, typeCount)
                        .Select(index => new ZLinkObjectCapability(
                            ZLinkPlacementObjectKind.InstanceSpot,
                            index == 0 ? "remote" : "other",
                            ZLinkObjectMaintenancePolicyKind.Disabled,
                            false,
                            0
                        ))
                        .ToArray(),
                },
            ];
        }
    }

    [Theory]
    [InlineData(false, true)]
    [InlineData(true, true)]
    [InlineData(true, false)]
    public async Task Local_instance_send_does_not_return_target_activation_deadline_failure(
        bool activationDeadlineExpires,
        bool inboundFlow
    )
    {
        const string spotId = "local-cold-send-deadline";
        HeldInstanceResolveStore? heldStore = null;
        var probe = new ColdSendProbe();
        var relocationStore = new ObservedActivationRelocationStore();
        using var observer = new CapturingMessageFlowObserver();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            messageFlowObserver: observer,
            messageFlowMode: ZLinkDiagnosticsLevel.Normal,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            instanceSpotType: typeof(ColdSendInstanceSpot),
            instanceDispatchProbe: probe,
            locationStoreWrapper: inner => heldStore = new(inner, spotId),
            relocationStore: relocationStore
        );
        runtime.Registration.SpotNodes["entry"].DefaultRequestTimeout = activationDeadlineExpires
            ? TimeSpan.FromMilliseconds(100)
            : TimeSpan.FromSeconds(5);
        var targetFailure = new TaskCompletionSource<Exception>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        runtime.ErrorSink.UnhandledCallbackException += error => targetFailure.TrySetResult(error);
        var flowId = ZlinkStreamFlowId.Create();
        try
        {
            Task pending;
            using (
                inboundFlow
                    ? ZLinkFlowContext.EnterExisting(flowId, ZLinkFlowOrigin.Inbound)
                    : default
            )
                pending = new ZLinkSpotClient(runtime)
                    .SendToSpot(spotId, new ProbeRouteMessage("activate"))
                    .InstanceSpot()
                    .Async()
                    .AsTask();
            await heldStore!.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            if (activationDeadlineExpires)
                await Task.Delay(TimeSpan.FromMilliseconds(350));
            Assert.False(pending.IsCompleted);
            heldStore.Release.TrySetResult();
            await pending.WaitAsync(TimeSpan.FromSeconds(5));
            if (activationDeadlineExpires)
            {
                var error = Assert.IsType<ZLinkFrameworkException>(
                    await targetFailure.Task.WaitAsync(TimeSpan.FromSeconds(5))
                );
                Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
                Assert.Equal(
                    "timed_out",
                    Assert
                        .Single(
                            runtime.GetSpotNodeRuntime("entry").GetInstanceSpotMonitoringSnapshots()
                        )
                        .LastActivationOutcome
                );
                var dropped = Assert.Single(
                    observer.Events,
                    flow => flow.SpotId == spotId && flow.Phase == "dropped"
                );
                Assert.Equal("instance_spot", dropped.Surface);
                Assert.Equal("activation_timeout", dropped.Reason);
                if (inboundFlow)
                    Assert.Equal(flowId, dropped.FlowId);
                else
                    Assert.True(ZlinkStreamFlowId.IsValid(dropped.FlowId));
                Assert.Equal(inboundFlow ? "inbound" : "application", dropped.FlowOrigin);
                Assert.Equal(error.GetType().Name, dropped.ErrorType);
                Assert.Equal(error.Message, dropped.ErrorMessage);
            }
            else
            {
                Assert.Equal(
                    "activate",
                    await probe.Message.Task.WaitAsync(TimeSpan.FromSeconds(5))
                );
                Assert.False(probe.Release.Task.IsCompleted);
                Assert.False(targetFailure.Task.IsCompleted);
                probe.Release.TrySetResult();
                await relocationStore.Deleted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            }
            await runtime.StopAsync(CancellationToken.None);
            Assert.Equal(!activationDeadlineExpires, probe.Message.Task.IsCompleted);
            Assert.Equal(activationDeadlineExpires ? 0 : 1, probe.HandlerCalls);
        }
        finally
        {
            heldStore?.Release.TrySetResult();
            probe.Release.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class ColdSendInstanceSpot(IZLinkInstanceSpotContext context)
        : IZLinkInstanceSpot
    {
        public IZLinkInstanceSpotContext Context { get; } = context;

        public void Configure() => Context.Handlers.AddPacket<ColdSendInstanceHandler>();
    }

    [Fact]
    public async Task Instance_activation_initialization_rejection_records_one_drop()
    {
        const string spotId = "initialization-terminal";
        var probe = new ColdSendProbe();
        using var observer = new CapturingMessageFlowObserver();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            messageFlowObserver: observer,
            messageFlowMode: ZLinkDiagnosticsLevel.Normal,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            instanceSpotType: typeof(FailingInitializationInstanceSpot),
            instanceDispatchProbe: probe,
            relocationStore: new InMemoryRelocationStore()
        );
        var failed = new TaskCompletionSource<Exception>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        runtime.ErrorSink.UnhandledCallbackException += error => failed.TrySetResult(error);
        var nodeRuntime = runtime.GetSpotNodeRuntime("entry");
        var flowId = ZlinkStreamFlowId.Create();
        try
        {
            using (ZLinkFlowContext.EnterExisting(flowId, ZLinkFlowOrigin.Inbound))
                await new ZLinkSpotClient(runtime)
                    .SendToSpot(spotId, new ProbeRouteMessage("activate"))
                    .InstanceSpot()
                    .Async();
            await probe.Message.Task.WaitAsync(TimeSpan.FromSeconds(5));
            probe.Release.SetResult();
            var error = await failed.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.IsType<InvalidOperationException>(error);
            Assert.Equal(
                "rejected",
                Assert
                    .Single(nodeRuntime.GetInstanceSpotMonitoringSnapshots())
                    .LastActivationOutcome
            );
            var dropped = Assert.Single(
                observer.Events,
                flow => flow.SpotId == spotId && flow.Phase == "dropped"
            );
            Assert.Equal("instance_spot", dropped.Surface);
            Assert.Equal("activation_rejected", dropped.Reason);
            Assert.Equal(flowId, dropped.FlowId);
            Assert.Equal("inbound", dropped.FlowOrigin);
            Assert.Equal(error.GetType().Name, dropped.ErrorType);
            Assert.Equal(error.Message, dropped.ErrorMessage);
        }
        finally
        {
            probe.Release.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class FailingInitializationInstanceSpot(
        IZLinkInstanceSpotContext context,
        ColdSendProbe probe
    ) : IZLinkInstanceSpot
    {
        public IZLinkInstanceSpotContext Context { get; } = context;

        public async ValueTask OnInitializeAsync(CancellationToken cancellationToken)
        {
            probe.Message.SetResult("initializing");
            await probe.Release.Task.WaitAsync(cancellationToken);
            throw new InvalidOperationException("Instance initialization rejected.");
        }
    }

    [Theory]
    [InlineData(ZLinkDiagnosticsLevel.Off, true)]
    [InlineData(ZLinkDiagnosticsLevel.Normal, true)]
    [InlineData(ZLinkDiagnosticsLevel.Normal, false)]
    public async Task Instance_activation_target_distinguishes_expired_deadline_from_shutdown(
        ZLinkDiagnosticsLevel level,
        bool cancelledShutdown
    )
    {
        const string spotId = "cancelled-activation";
        var node = new CapturingInstanceActivationNode();
        using var observer = new CapturingMessageFlowObserver();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            messageFlowObserver: observer,
            messageFlowMode: level,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            relocationStore: new InMemoryRelocationStore()
        );
        var shutdownToken = runtime.ShutdownToken;
        if (cancelledShutdown)
        {
            await runtime.StopAsync(CancellationToken.None);
            Assert.True(shutdownToken.IsCancellationRequested);
        }
        var flowId = ZlinkStreamFlowId.Create();
        using var header = ZLinkEnvelopeCodec.EncodeHeader(
            ZLinkClientCallCodec.CreateEnvelope(
                ZLinkMessageKind.Command,
                "entry",
                "ProbeRouteMessage"
            ) with
            {
                FlowId = flowId,
                FlowOrigin = ZLinkFlowOrigin.Inbound,
            }
        );
        var operation = new InstanceSpotActivationOperation(
            new InstanceSpotActivationTarget(
                "entry",
                node.RoutingId,
                node.MeshStatus().LifecycleGeneration,
                spotId,
                "Tests.InstanceSpot",
                "1"
            ),
            node.RoutingId,
            node.MeshStatus().LifecycleGeneration,
            string.Empty,
            new MeshOperationId(1, 2),
            false,
            0,
            checked(
                (ulong)
                    DateTimeOffset
                        .UtcNow.AddSeconds(cancelledShutdown ? 1 : -1)
                        .ToUnixTimeMilliseconds()
            )
        );
        try
        {
            Exception error;
            if (cancelledShutdown)
            {
                var cancelled = await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                    node.Target!.ActivateAsync(operation, null, [header.ToArray()], shutdownToken)
                        .AsTask()
                );
                Assert.True(cancelled.CancellationToken.IsCancellationRequested);
                error = cancelled;
            }
            else
            {
                var expired = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                    node.Target!.ActivateAsync(operation, null, [header.ToArray()], shutdownToken)
                        .AsTask()
                );
                Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, expired.Kind);
                Assert.False(shutdownToken.IsCancellationRequested);
                error = expired;
            }
            Assert.Equal(
                cancelledShutdown ? "shutdown" : "timed_out",
                ((ZLinkInstanceSpotActivationTarget)node.Target!)
                    .MonitoringSnapshot("Tests.InstanceSpot")
                    .LastActivationOutcome
            );
            var drops = observer.Events.Where(flow =>
                flow.SpotId == spotId && flow.Phase == "dropped"
            );
            if (level == ZLinkDiagnosticsLevel.Off)
                Assert.Empty(drops);
            else
            {
                var dropped = Assert.Single(drops);
                Assert.Equal("instance_spot", dropped.Surface);
                Assert.Equal(
                    cancelledShutdown ? "activation_rejected" : "activation_timeout",
                    dropped.Reason
                );
                Assert.Equal(flowId, dropped.FlowId);
                Assert.Equal("inbound", dropped.FlowOrigin);
                Assert.Equal(error.GetType().Name, dropped.ErrorType);
                Assert.Equal(error.Message, dropped.ErrorMessage);
            }
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class CapturingInstanceActivationNode : CapturingSpotNode, IZLinkBackendSpotNode
    {
        public IInstanceSpotActivationTarget? Target { get; private set; }

        void IZLinkBackendSpotNode.SetInstanceSpotActivationTarget(
            IInstanceSpotActivationTarget target
        ) => Target = target;
    }

    private sealed class ColdSendProbe
    {
        public TaskCompletionSource<string> Message { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public int HandlerCalls;
    }

    private sealed class ObservedActivationRelocationStore : IZLinkRelocationStore
    {
        private readonly InMemoryRelocationStore inner = new();
        public TaskCompletionSource Deleted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ValueTask<ZLinkBlobPutResult> PutAsync(
            ZLinkBlobReference reference,
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default
        ) => inner.PutAsync(reference, payload, retention, cancellationToken);

        public ValueTask<ZLinkBlobReadResult> ReadAsync(
            ZLinkBlobReference reference,
            CancellationToken cancellationToken = default
        ) => inner.ReadAsync(reference, cancellationToken);

        public ValueTask<ZLinkBlobRenewResult> RenewAsync(
            ZLinkBlobReference reference,
            TimeSpan retention,
            CancellationToken cancellationToken = default
        ) => inner.RenewAsync(reference, retention, cancellationToken);

        public async ValueTask DeleteAsync(
            ZLinkBlobReference reference,
            CancellationToken cancellationToken = default
        )
        {
            await inner.DeleteAsync(reference, cancellationToken);
            Deleted.TrySetResult();
        }
    }

    private sealed class ColdSendInstanceHandler(ColdSendProbe probe)
        : IZLinkSpotPacketHandler<ColdSendInstanceSpot, ProbeRouteMessage>
    {
        public async ValueTask HandleAsync(
            ColdSendInstanceSpot spot,
            ProbeRouteMessage message,
            CancellationToken cancellationToken
        )
        {
            Interlocked.Increment(ref probe.HandlerCalls);
            probe.Message.SetResult(message.Value);
            await probe.Release.Task.WaitAsync(cancellationToken);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Instance_send_fixes_activation_deadline_before_resolve_without_ending_caller_wait(
        bool inferInstanceType
    )
    {
        const string spotId = "cold-send-deadline";
        var node = new PendingInstanceSendNode();
        HeldInstanceResolveStore? heldStore = null;
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            includeInstanceSpotRoute: true,
            locationStoreWrapper: inner => heldStore = new(inner, spotId),
            relocationStore: new InMemoryRelocationStore(),
            meshResolverWrapper: inner => new RemoteInstanceResolver(inner)
        );
        var timeout = TimeSpan.FromMilliseconds(100);
        runtime.Registration.SpotNodes["entry"].DefaultRequestTimeout = timeout;
        try
        {
            await PublishCandidateAsync(
                RequireLocationStore(runtime),
                "remote-instance-owner",
                RoutingId.From("remote-instance-node"),
                TimeSpan.FromMinutes(1)
            );
            var client = new ZLinkSpotClient(runtime);
            var call = inferInstanceType
                ? client.SendToSpot(spotId, new ProbeRouteMessage("activate")).InstanceSpot()
                : client
                    .SendToSpot(spotId, new ProbeRouteMessage("activate"))
                    .InstanceSpot("Tests.InstanceSpot")
                    .InMesh("entry");
            var before = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            var pending = call.Async().AsTask();
            var after = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            await heldStore!.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await Task.Delay(TimeSpan.FromMilliseconds(350));
            Assert.False(pending.IsCompleted);
            heldStore.Release.TrySetResult();
            await node.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            try
            {
                Assert.InRange(
                    node.DeadlineUnixMs,
                    (ulong)(before + timeout.TotalMilliseconds),
                    (ulong)(after + timeout.TotalMilliseconds)
                );
                Assert.True(
                    node.DeadlineUnixMs < (ulong)DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
                );
                await Task.Delay(TimeSpan.FromMilliseconds(350));
                Assert.False(pending.IsCompleted);
                Assert.False(node.Token.IsCancellationRequested);
            }
            finally
            {
                node.Release.TrySetResult();
            }
            await pending.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(1, node.Submissions);
        }
        finally
        {
            heldStore?.Release.TrySetResult();
            node.Release.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class PendingInstanceSendNode : CapturingSpotNode, IZLinkBackendSpotNode
    {
        public TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public ulong DeadlineUnixMs { get; private set; }
        public CancellationToken Token { get; private set; }
        public int Submissions { get; private set; }
        internal string? StableType { get; private set; }

        public async ValueTask<IReadOnlyList<Message>> ActivateInstanceSpotAsync(
            InstanceSpotActivationTarget target,
            string sourceSpotId,
            IReadOnlyList<Message> parts,
            bool request,
            ulong deadlineUnixMs,
            TimeSpan timeout,
            ReadOnlyMemory<byte> metadata,
            CancellationToken cancellationToken,
            Zlink.Framework.Runtime.Messaging.ZLinkRequestAdmission? requestAdmission = null
        )
        {
            Assert.False(request);
            StableType = target.StableType;
            DeadlineUnixMs = deadlineUnixMs;
            Token = cancellationToken;
            Submissions++;
            Entered.TrySetResult();
            await Release.Task.WaitAsync(cancellationToken);
            return [];
        }
    }

    private sealed class RemoteInstanceResolver(IZLinkMeshNodeLocationResolver inner)
        : IZLinkMeshNodeLocationResolver
    {
        public async ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
            string meshName,
            CancellationToken cancellationToken = default
        ) =>
            (await inner.ListLiveMeshNodesAsync(meshName, cancellationToken))
                .Where(candidate => candidate.Rid == RoutingId.From("remote-instance-node"))
                .ToArray();
    }

    private sealed class HeldInstanceResolveStore(IZLinkLocationStore inner, string spotId)
        : IZLinkLocationStore
    {
        public TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        )
        {
            if (
                key
                == ZLinkProviderLocationRepository.AuthorityMetaKey(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId)
                )
            )
            {
                Entered.TrySetResult();
                await Release.Task.WaitAsync(cancellationToken);
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
}
