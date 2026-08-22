using Microsoft.Extensions.DependencyInjection;
using System.Net;
using System.Net.Sockets;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class CanonicalActorJoinWireAdmissionNegativeTests
{
    [Theory]
    [InlineData(ActorFenceMismatch.ObjectGeneration)]
    [InlineData(ActorFenceMismatch.NodeRid)]
    [InlineData(ActorFenceMismatch.NodeGeneration)]
    public async Task Canonical_actor_join_authority_fence_mismatch_returns_location_stale(
        ActorFenceMismatch mismatch)
    {
        await using var fixture = await WireAdmissionFixture.CreateAsync(mismatch);
        var request = fixture.CreateRequest();
        if (mismatch == ActorFenceMismatch.ObjectGeneration)
            request = request with
            {
                Actor = request.Actor with
                {
                    ObjectGeneration = checked(request.Actor.ObjectGeneration + 1)
                }
            };

        var reply = await fixture.SendAsync(request);

        AssertTerminal(
            reply,
            RequestResult.Conflict,
            ServiceWireConstants.FrameworkErrorCode.ActorLocationStale);
    }

    [Fact]
    public async Task Canonical_actor_join_store_resolved_type_mismatch_returns_actor_type_mismatch()
    {
        await using var fixture = await WireAdmissionFixture.CreateAsync();
        var state = fixture.Runtime.GetOrCreateActorState(WireAdmissionFixture.ActorId);
        var operation = await state.GetOrStartActorCreationAsync(
            "other-actor-type",
            failIfExists: false,
            () => Task.FromResult<IZLinkActor>(new TestActor(WireAdmissionFixture.ActorId)),
            CancellationToken.None);
        _ = await operation.Task;

        var reply = await fixture.SendAsync(fixture.CreateRequest());

        AssertTerminal(
            reply,
            RequestResult.Conflict,
            ServiceWireConstants.FrameworkErrorCode.ActorTypeMismatch);
    }

    [Fact]
    public async Task Canonical_actor_join_malformed_body_returns_protocol_error()
    {
        await using var fixture = await WireAdmissionFixture.CreateAsync();
        var request = fixture.CreateRequest();
        var malformed = ZLinkServiceWireCodec.EncodeActorJoinRequest(request);
        Array.Resize(ref malformed, malformed.Length - 1);
        Assert.False(ZLinkServiceWireCodec.TryDecodeActorJoinRequest(
            malformed,
            "wire-admission",
            out _,
            out _));

        var reply = await fixture.SendAsync(request, malformed);

        AssertTerminal(
            reply,
            RequestResult.ProtocolError,
            ServiceWireConstants.FrameworkErrorCode.RequestProtocolError);
    }

    [Fact]
    public async Task Canonical_actor_join_full_mailbox_returns_backpressured_terminal()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var target = new ZLinkManagedMeshNode(context, "wire-admission");
        var suffix = Guid.NewGuid().ToString("N");
        var targetRid = RoutingId.From($"target-{suffix}");
        var endpoint = WireAdmissionFixture.AllocateTcpEndpoint();
        target.SetRoutingId(targetRid);
        target.SetObjectRole(ZLinkMeshNodeObjectRole.Client);
        target.SetBind(endpoint);
        target.MailboxMessageBudget = 1;
        var targetSpot = (ZLinkManagedSpot)target.GetOrCreateSpot(
            "target-spot",
            out _);
        target.Start();
        var targetNodeGeneration = target.Status().LifecycleGeneration;

        await using var firstSource = context.CreateDealerSocket();
        var firstRid = RoutingId.From($"first-{suffix}");
        firstSource.SetRoutingId(firstRid);
        firstSource.Connect(endpoint);
        await WireAdmissionFixture.SendHelloAsync(
            firstSource,
            "wire-admission",
            $"inproc://first-{suffix}");
        await WireAdmissionFixture.WaitUntilAsync(
            () => target.Status().AdmittedPeerCount == 1);
        _ = SendRequestAsync(
            firstSource,
            CreateDirectRequest(
                firstRid,
                targetRid,
                targetNodeGeneration,
                targetSpot.LifecycleGeneration,
                correlation: 201));
        await Task.Delay(100);

        await using var secondSource = context.CreateDealerSocket();
        var secondRid = RoutingId.From($"second-{suffix}");
        secondSource.SetRoutingId(secondRid);
        secondSource.Connect(endpoint);
        await WireAdmissionFixture.SendHelloAsync(
            secondSource,
            "wire-admission",
            $"inproc://second-{suffix}");
        await WireAdmissionFixture.WaitUntilAsync(
            () => target.Status().AdmittedPeerCount == 2);

        var reply = await SendRequestAsync(
            secondSource,
            CreateDirectRequest(
                secondRid,
                targetRid,
                targetNodeGeneration,
                targetSpot.LifecycleGeneration,
                correlation: 202));

        AssertTerminal(
            reply,
            RequestResult.Backpressured,
            ServiceWireConstants.FrameworkErrorCode.None);
    }

    private static ActorJoinRequest CreateDirectRequest(
        RoutingId sourceRid,
        RoutingId targetRid,
        ulong targetNodeGeneration,
        ulong targetSpotGeneration,
        ulong correlation) => new(
        correlation,
        new ActorRef("actor-1", 1, "wire-admission", sourceRid),
        1,
        1,
        1,
        false,
        "target-spot",
        targetSpotGeneration,
        targetRid,
        targetNodeGeneration,
        1,
        1);

    private static Task<IReadOnlyList<Message>> SendRequestAsync(
        IDealerSocket source,
        ActorJoinRequest request)
    {
        using var head = Message.From(
            ZLinkServiceWireCodec.EncodeActorJoinRequest(request));
        using var payload = Message.From(
            ZLinkApplicationPayloadEnvelopeCodec.Encode(
                "JoinRequest",
                "application/json",
                "{}"u8));
        return source.Request()
            .Message(head)
            .Message(payload)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async(CancellationToken.None);
    }

    private static void AssertTerminal(
        IReadOnlyList<Message> parts,
        RequestResult expectedResult,
        ServiceWireConstants.FrameworkErrorCode expectedCode)
    {
        try
        {
            var part = Assert.Single(parts);
            Assert.True(ZLinkServiceWireCodec.TryDecodeReply(
                part.AsReadOnlyMemory().Span,
                out var reply,
                out var error));
            Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
            Assert.Equal((int)expectedResult, reply.TerminalResult);
            Assert.Equal((uint)expectedCode, reply.FailureCode);
            Assert.Empty(reply.Tail);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    public enum ActorFenceMismatch
    {
        ObjectGeneration,
        NodeRid,
        NodeGeneration
    }

    private sealed class WireAdmissionFixture : IAsyncDisposable
    {
        private const string MeshName = "wire-admission";
        internal const string ActorId = "actor-1";
        private const string TargetSpotId = "target-spot";
        private const string ActorType = "actor-type";
        private readonly IContext _sourceContext;
        private readonly IDealerSocket _source;
        private readonly ServiceProvider _services;
        private readonly RoutingId _sourceRid;
        private readonly RoutingId _targetRid;
        private readonly ulong _targetNodeGeneration;
        private readonly ZLinkAuthoritySnapshot _actorAuthority;
        private readonly ZLinkAuthoritySnapshot _targetSpotAuthority;

        private WireAdmissionFixture(
            IContext sourceContext,
            IDealerSocket source,
            ServiceProvider services,
            ZLinkFrameworkRuntime runtime,
            RoutingId sourceRid,
            RoutingId targetRid,
            ulong targetNodeGeneration,
            ZLinkAuthoritySnapshot actorAuthority,
            ZLinkAuthoritySnapshot targetSpotAuthority)
        {
            _sourceContext = sourceContext;
            _source = source;
            _services = services;
            Runtime = runtime;
            _sourceRid = sourceRid;
            _targetRid = targetRid;
            _targetNodeGeneration = targetNodeGeneration;
            _actorAuthority = actorAuthority;
            _targetSpotAuthority = targetSpotAuthority;
        }

        internal ZLinkFrameworkRuntime Runtime { get; }

        internal static async Task<WireAdmissionFixture> CreateAsync(
            ActorFenceMismatch? mismatch = null)
        {
            var provider = new ZLinkInMemoryProviderLocationStore();
            var store = new ZLinkProviderLocationRepository(provider);
            var endpoint = AllocateTcpEndpoint();
            var registration = new ZLinkFrameworkRegistration
            {
                DefaultRequestTimeout = TimeSpan.FromSeconds(2),
                ImplicitHandlerAutoRegistrationEnabled = false
            };
            registration.Locations.StoreInstance = provider;
            var locationOptions = new ZLinkLocationOptions
            {
                PollingInterval = TimeSpan.Zero
            };
            var locationRuntime = new ZLinkLocationRuntime(
                locationOptions,
                store);
            Assert.True(await locationRuntime.RenewOwnerLeaseOnceAsync());
            var locationResolvers = new ZLinkStoreLocationResolvers(
                store,
                new ZLinkOwnerLeaseTracker(store, locationOptions),
                new ZLinkObservedLocationGenerations(),
                options: locationOptions);
            var locationLifecycle = new ZLinkLocationLifecycle(
                locationRuntime,
                locationResolvers);
            var services = new ServiceCollection()
                .AddSingleton(locationRuntime)
                .AddSingleton(locationLifecycle)
                .AddSingleton(locationResolvers)
                .AddSingleton(new ZLinkLocationAddressResolvers(
                    locationResolvers,
                    new ZLinkSpotHandleRegistry()))
                .AddSingleton(new ZLinkSpotRetireTargetRuntime(
                    null!,
                    null!,
                    registration))
                .BuildServiceProvider();
            var node = new ZLinkSpotNodeRegistration
            {
                SpotNodeName = MeshName,
                ObjectRole = ZLinkMeshNodeObjectRole.Server,
                ObjectRoleSelected = true,
                Router = new ZLinkSpotRouterCapabilityRegistration
                {
                    BindEndpoint = endpoint,
                    AcquisitionMode = ZLinkPeerAcquisitionMode.Manual
                }
            };
            node.SpotFactories.Add(typeof(AdmissionSpot));
            node.UserSpotFactoryOptions[typeof(AdmissionSpot)] =
                new ZLinkUserSpotFactoryConfiguration();
            node.SpotRelocations[typeof(AdmissionSpot).FullName!] =
                new ZLinkObjectRelocationRegistration(
                    typeof(AdmissionSpot),
                    new ZLinkObjectPlacementOptions(),
                    PolicyKind: 1,
                    AdapterType: null,
                    AdapterInvoker: null);
            node.ActorRelocations[ActorType] = new ZLinkObjectRelocationRegistration(
                typeof(TestActor),
                new ZLinkObjectPlacementOptions(),
                PolicyKind: 1,
                AdapterType: null,
                AdapterInvoker: null);
            registration.SpotNodes[MeshName] = node;
            registration.ActorCatalog.Build(registration.SpotNodes.Values);
            var runtime = new ZLinkFrameworkRuntime(
                services,
                new ZLinkDotNetBackendAdapterFactory(),
                registration,
                new ZLinkHandlerRegistry([]),
                new ZLinkHandlerDispatcher(
                    services.GetRequiredService<IServiceScopeFactory>(),
                    registration));
            await runtime.StartAsync(CancellationToken.None);
            try
            {
                var targetNode = runtime.GetSpotNodeRuntime(MeshName).Node;
                var targetRid = targetNode.RoutingId;
                var targetNodeGeneration = targetNode.MeshStatus().LifecycleGeneration;
                var descriptor = (await store.ListMeshNodesAsync(
                        MeshName,
                        default))
                    .Items
                    .Single(item => item.Rid == targetRid);
                Assert.Equal(
                    ZLinkLocationWriteStatus.Stored,
                    (await store.UpdateMeshNodeAsync(
                        descriptor with
                        {
                            State = ZLinkFrameworkRuntimeState.Serving,
                            DescriptorRevision = checked(
                                descriptor.DescriptorRevision + 1)
                        },
                        ZLinkLocationWriteIntent.Renew)).Status);
                await runtime.GetOrCreateAsync<AdmissionSpot>(TargetSpotId);
                var targetSpotAuthority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                    await store.ReadAuthorityAsync(
                        ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(TargetSpotId))).Snapshot;

                var sourceOwner = $"source-owner-{Guid.NewGuid():N}";
                await store.ClaimLiveOwnerAsync(sourceOwner, TimeSpan.FromMinutes(5));
                var sourceRid = RoutingId.From($"source-{Guid.NewGuid():N}");
                var actorRow = InMemoryLocationStoreTests.Actor(sourceOwner, ActorId) with
                {
                    MeshName = MeshName,
                    ActorType = ActorType,
                    ActorRef = new ActorRef(ActorId, 1, MeshName, sourceRid),
                    OwnerNodeRid = mismatch == ActorFenceMismatch.NodeRid
                        ? RoutingId.From($"different-source-{Guid.NewGuid():N}")
                        : sourceRid,
                    OwnerNodeGeneration = mismatch == ActorFenceMismatch.NodeGeneration
                        ? 2UL
                        : 1UL
                };
                var actorAuthority = Assert.IsType<ZLinkAuthoritySnapshot>(
                    await AuthorityLocationTestFixture.PublishActorAsync(
                        store,
                        actorRow));

                var sourceContext = Systems.Zlink.Zlink.CreateContext();
                var source = sourceContext.CreateDealerSocket();
                source.SetRoutingId(sourceRid);
                source.Connect(endpoint);
                await SendHelloAsync(
                    source,
                    MeshName,
                    $"inproc://{sourceOwner}");
                await WaitUntilAsync(() => targetNode.MeshStatus().AdmittedPeerCount == 1);
                return new WireAdmissionFixture(
                    sourceContext,
                    source,
                    services,
                    runtime,
                    sourceRid,
                    targetRid,
                    targetNodeGeneration,
                    actorAuthority,
                    targetSpotAuthority);
            }
            catch
            {
                await runtime.StopAsync(CancellationToken.None);
                await services.DisposeAsync();
                throw;
            }
        }

        internal ActorJoinRequest CreateRequest()
        {
            var actorGeneration = _actorAuthority.ObjectGeneration;
            if (_actorAuthority.ObjectGeneration == 0)
                throw new InvalidOperationException("Actor authority generation is absent.");
            return new ActorJoinRequest(
                101,
                new ActorRef(ActorId, actorGeneration, MeshName, _sourceRid),
                ActorNodeGeneration: 1,
                ActorAuthorityOwnerGeneration: _actorAuthority.AuthorityOwnerGeneration,
                ActorOwnerLeaseGeneration: checked((ulong)_actorAuthority.OwnerLeaseGeneration),
                Entry: false,
                TargetSpotId,
                _targetSpotAuthority.ObjectGeneration,
                _targetRid,
                _targetNodeGeneration,
                _targetSpotAuthority.AuthorityOwnerGeneration,
                checked((ulong)_targetSpotAuthority.OwnerLeaseGeneration));
        }

        internal async Task<IReadOnlyList<Message>> SendAsync(
            ActorJoinRequest request,
            byte[]? head = null)
        {
            using var requestHead = Message.From(
                head ?? ZLinkServiceWireCodec.EncodeActorJoinRequest(request));
            using var payload = Message.From(
                ZLinkApplicationPayloadEnvelopeCodec.Encode(
                    "JoinRequest",
                    "application/json",
                    "{}"u8));
            return await _source.Request()
                .Message(requestHead)
                .Message(payload)
                .Timeout(TimeSpan.FromSeconds(2))
                .Async(CancellationToken.None);
        }

        public async ValueTask DisposeAsync()
        {
            await _source.DisposeAsync();
            await _sourceContext.DisposeAsync();
            await Runtime.StopAsync(CancellationToken.None);
            await _services.DisposeAsync();
        }

        internal static async Task SendHelloAsync(
            IDealerSocket source,
            string meshName,
            string sourceEndpoint)
        {
            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(2);
            while (true)
            {
                try
                {
                    using var hello = Message.From(
                        ZLinkServiceWireCodec.EncodeRouteAdmission(
                            ServiceWireConstants.Command.Hello,
                            meshName,
                            sourceEndpoint,
                            lifecycleGeneration: 1,
                            descriptorRevision: 1,
                            new Dictionary<string, uint>(StringComparer.Ordinal),
                            objectRole: (byte)ZLinkMeshNodeObjectRole.Server));
                    await source.Send().Message(hello).Async(CancellationToken.None);
                    return;
                }
                catch (ZlinkSubmitException) when (DateTime.UtcNow < deadline)
                {
                    await Task.Delay(10);
                }
            }
        }

        internal static async Task WaitUntilAsync(Func<bool> condition)
        {
            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(2);
            while (!condition())
            {
                if (DateTime.UtcNow >= deadline)
                    throw new TimeoutException("Wire admission setup did not complete.");
                await Task.Delay(10);
            }
        }

        internal static string AllocateTcpEndpoint()
        {
            using var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            var endpoint = Assert.IsType<IPEndPoint>(listener.LocalEndpoint);
            return $"tcp://127.0.0.1:{endpoint.Port}";
        }
    }

    private sealed class AdmissionSpot(IZLinkSpotContext context) : IZLinkSpot<TestActor>
    {
        public IZLinkSpotContext Context { get; } = context;

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken) =>
            ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());

        public ValueTask OnJoinedActorAsync(
            TestActor actor,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(
            TestActor actor,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }
}
