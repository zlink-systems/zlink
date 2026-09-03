using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Diagnostics.Metrics;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Fact]
    public void Missing_actor_factory_is_not_found_and_is_not_retryable()
    {
        var registration = new ZLinkFrameworkRegistration();
        registration.ActorCatalog.Build([]);

        var error = Assert.Throws<ZLinkFrameworkException>(
            () => registration.ActorCatalog.ResolveFactory("unregistered"));

        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
        Assert.Equal(ZLinkRetryAdvice.DoNotRetry, error.RetryAdvice);
    }

    [Fact]
    public async Task BoundSessionAsyncSend_UsesCommittedRelocationRouteBeforeAck()
    {
        var localNode = new CapturingSpotNode();
        localNode.SetRoutingId(RoutingId.From("target-node"));
        var state = new ZLinkActorRuntimeState("actor-1");
        var sessionNode = RoutingId.From("session-node");
        var sessionRid = RoutingId.From("session-rid");
        state.StageRelocationSessionRoute(
            "handoff-1",
            new ZLinkRemoteActorBoundSessionRoute(
                sessionNode,
                sessionRid,
                "binding-1",
                BindingGeneration: 4,
                ObjectGeneration: 7,
                AuthorityOwnerGeneration: 11,
                MeshName: "play",
                TargetNodeGeneration: 4,
                OwnerLeaseGeneration: 8,
                SessionOwnerNodeGeneration: 3,
                AcceptedHighWater: 9));
        state.MarkRelocationSessionAuthorityCommitted(
            "handoff-1",
            new ZLinkBackendActorRef(
                RoutingId.From("target-node"), "actor-1", 7),
            targetAuthorityOwnerGeneration: 12,
            targetMeshName: ZLinkMeshName.FromBoundary("play", "targetMeshName"),
            targetNodeGeneration: 5,
            targetOwnerLeaseGeneration: 9);

        var relays = 0;
        ZLinkActorBoundSession? selected = null;
        var coordinator = new ZLinkActorBoundSessionCoordinator(
            actorId =>
            {
                Assert.Equal("actor-1", actorId);
                return state;
            },
            () => localNode,
            _ => localNode,
            new ZLinkFrameworkRegistration(),
            () => CancellationToken.None)
        {
            RemotePushRelayAsync = (_, session, _, _) =>
            {
                relays++;
                selected = session;
                return ValueTask.FromResult(new ZLinkOneWaySubmitResult(
                    ZLinkOneWaySubmitStatus.Submitted));
            }
        };

        using var frame = Message.From("join-notify");
        var result = await coordinator.SendIfBoundToAsync(
            "actor-1",
            "binding-1",
            [frame],
            CancellationToken.None);

        Assert.Equal(ZLinkOneWaySubmitStatus.Submitted, result.Status);
        Assert.Equal(1, relays);
        var route = Assert.IsType<ZLinkActorBoundSession>(selected);
        Assert.Equal(sessionNode, route.SessionNodeRid);
        Assert.Equal(sessionRid, route.SessionRid);
        Assert.Equal((ulong)12, route.AuthorityOwnerGeneration);
        Assert.Equal((ulong)5, route.TargetNodeGeneration);
        Assert.Equal((ulong)9, route.OwnerLeaseGeneration);
    }

    [Fact]
    public void SpotActivationTypes_ExposeOnlyTheirLifecycleContext()
    {
        Assert.True(typeof(ZLinkSpotActivation).IsAbstract);
        Assert.True(typeof(IZLinkSpotContext).IsAssignableFrom(
            typeof(ZLinkUserSpotActivation)));
        Assert.False(typeof(IZLinkInstanceSpotContext).IsAssignableFrom(
            typeof(ZLinkUserSpotActivation)));
        Assert.True(typeof(IZLinkInstanceSpotContext).IsAssignableFrom(
            typeof(ZLinkInstanceSpotActivation)));
        Assert.False(typeof(IZLinkSpotContext).IsAssignableFrom(
            typeof(ZLinkInstanceSpotActivation)));
        Assert.True(typeof(IZLinkSpotHandlerRegistrySink).IsAssignableFrom(
            typeof(ZLinkUserSpotActivation)));
        Assert.False(typeof(IZLinkSpotHandlerRegistrySink).IsAssignableFrom(
            typeof(ZLinkInstanceSpotActivation)));

        Assert.Contains(
            typeof(ZLinkUserSpotActivation).GetMethods(),
            static method => method.Name == nameof(IZLinkSpotContext.RelocationReady));
        Assert.DoesNotContain(
            typeof(ZLinkInstanceSpotActivation).GetMethods(),
            static method => method.Name == nameof(IZLinkSpotContext.RelocationReady));
    }

    [Fact]
    public async Task EntrySpot_Identity_Is_FrameworkIssued_After_Node_Bind()
    {
        var services = new ServiceCollection().BuildServiceProvider();
        var node = new CapturingSpotNode();
        var registration = new ZLinkFrameworkRegistration();
        registration.SpotNodes["entry"] = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "entry",
            RoutingId = RoutingId.From("entry-node"),
            Router = new ZLinkSpotRouterCapabilityRegistration
            {
                BindEndpoint = "inproc://entry"
            },
        };
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new CapturingBackendAdapterFactory(node),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));

        await runtime.StartAsync(CancellationToken.None);
        try
        {
            Assert.Null(registration.SpotNodes["entry"].EntrySpotType);
            Assert.StartsWith(
                "entry-entry-",
                node.EntryRoutingId.ToString(),
                StringComparison.Ordinal);
            // RouteMesh 10.0.0 MeshNode startup ordering (spec 21-mesh-node §3):
            // routing id and object role, then ROUTER bind (+ channels + Start),
            // then entry-spot configuration. The 9.x order applied the entry-spot
            // routing id before the router bind; the node now binds/starts before
            // any entry-spot use.
            Assert.Equal(
                new[]
                {
                    "node-rid:entry-node",
                    "object-role:None",
                    "router-bind:inproc://entry",
                    "node-route-handler",
                    "node-start",
                    "entry-facade"
                },
                node.InitializationEvents.Take(6));
            Assert.StartsWith(
                "entry-rid:entry-entry-",
                node.InitializationEvents.ElementAt(6),
                StringComparison.Ordinal);
            Assert.Equal("ingress-activate", node.InitializationEvents.ElementAt(7));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Startup_prepares_immediate_Node_Channel_Actor_and_UserSpot_ingress()
    {
        var node = new CapturingSpotNode();
        Task<ActorCreateOperationTerminal>? actorIngress = null;
        Task<UserSpotOperationTerminal>? userSpotIngress = null;
        var nodeReply = new TaskCompletionSource<string>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var channelReply = new TaskCompletionSource<string>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        node.NativeIngressOnStart = () =>
        {
            var targetRid = RoutingId.From("entry-node");
            var reservation = new ObjectReservationFence(
                "startup-reservation",
                "startup-store-version",
                1,
                1,
                targetRid,
                1,
                "startup-owner",
                1,
                1);
            actorIngress = node.ActorCreateOperationTarget!.CreateAsync(
                    new ActorCreateOperation(
                        1,
                        new MeshOperationId(1, 1),
                        RoutingId.From("startup-source"),
                        1,
                        "startup-actor",
                        "startup-actor-type",
                        reservation,
                        ulong.MaxValue),
                    CancellationToken.None)
                .AsTask();
            userSpotIngress = node.UserSpotOperationTarget!.CreateAsync(
                    new UserSpotCreateOperation(
                        2,
                        new MeshOperationId(1, 2),
                        RoutingId.From("startup-source"),
                        1,
                        "startup-spot",
                        typeof(EmptyUserSpot).FullName!,
                        reservation,
                        ulong.MaxValue),
                    CancellationToken.None)
                .AsTask();
        };
        node.ApplicationIngressOnActivation = () =>
        {
            node.NodeRouteHandler!(CreateImmediateMeshRequest(
                value: "node",
                channelName: null,
                requestSequence: 11,
                nodeReply));
            node.NodeRouteHandler!(CreateImmediateMeshRequest(
                value: "channel",
                channelName: "startup-channel",
                requestSequence: 12,
                channelReply));
        };

        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            userSpotType: typeof(EmptyUserSpot),
            includeImmediateIngressHandlers: true);
        try
        {
            Assert.True(node.ActorTargetReadyAtStart);
            Assert.True(node.UserSpotTargetReadyAtStart);
            Assert.True(node.NodeRouteReadyAtStart);
            Assert.True(node.EntryDispatchReadyAtActivation);

            var events = node.InitializationEvents.ToArray();
            var start = Array.IndexOf(events, "node-start");
            var activate = Array.IndexOf(events, "ingress-activate");
            Assert.InRange(Array.IndexOf(events, "actor-create-target"), 0, start - 1);
            Assert.InRange(Array.IndexOf(events, "user-spot-target"), 0, start - 1);
            Assert.InRange(Array.IndexOf(events, "node-route-handler"), 0, start - 1);
            Assert.True(activate > start);

            var actorError = await Assert.ThrowsAsync<ZLinkFrameworkException>(
                async () => await actorIngress!);
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, actorError.Kind);
            var spotError = await Assert.ThrowsAsync<ZLinkFrameworkException>(
                async () => await userSpotIngress!);
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, spotError.Kind);
            Assert.Equal(
                "NODE",
                await nodeReply.Task.WaitAsync(TimeSpan.FromSeconds(5)));
            Assert.Equal(
                "CHANNEL",
                await channelReply.Task.WaitAsync(TimeSpan.FromSeconds(5)));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Automatic_Route_Target_Convergence_Is_Unavailable_Not_NotFound()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            topology: new TestRouteMeshTopology(
                ZLinkRouteMeshTargetClassification.Unknown,
                CompleteSnapshot: null),
            includeActorFactory: false);
        try
        {
            var error = Assert.Throws<ZLinkFrameworkException>(() =>
                runtime.EnsureKnownRouteMeshPeer(
                    "entry",
                    RoutingId.From("remote-node"),
                    "SPOT 'remote'"));

            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
            Assert.Equal(ZLinkRetryAdvice.RetryAfterBackoff, error.RetryAdvice);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Automatic_Route_Target_Absent_From_Complete_Snapshot_Is_NotFound()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            topology: new TestRouteMeshTopology(
                ZLinkRouteMeshTargetClassification.Unknown,
                CompleteSnapshot: Array.Empty<ZLinkRouteMeshPeerIdentity>()),
            includeActorFactory: false);
        try
        {
            var error = Assert.Throws<ZLinkFrameworkException>(() =>
                runtime.EnsureKnownRouteMeshPeer(
                    "entry",
                    RoutingId.From("remote-node"),
                    "SPOT 'remote'"));

            Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
            Assert.Equal(ZLinkRetryAdvice.DoNotRetry, error.RetryAdvice);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task SpotNode_Initializer_Applies_Router_Send_Config()
    {
        var services = new ServiceCollection().BuildServiceProvider();
        var node = new CapturingSpotNode();
        var registration = new ZLinkFrameworkRegistration();
        registration.SpotNodes["publisher"] = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "publisher",
            RoutingId = RoutingId.From("publisher-node"),
            Router = new ZLinkSpotRouterCapabilityRegistration
            {
                BindEndpoint = "inproc://publisher",
                SocketConfig =
                {
                    SendHighWaterMark = 17,
                    SendTimeout = TimeSpan.FromMilliseconds(21)
                }
            },
            ChannelMemberships =
            {
                new ZLinkMeshChannelMembership { ChannelName = "events" }
            }
        };
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new CapturingBackendAdapterFactory(node),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));

        await runtime.StartAsync(CancellationToken.None);
        try
        {
            Assert.Equal(17UL, node.RouterHighWaterMark);
            Assert.Equal(TimeSpan.FromMilliseconds(21), node.RouterSendTimeout);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task RouteClient_NodeSubmitAsync_Forwards_The_Metadata_Snapshot()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        try
        {
            var client = new ZLinkRouteClient(runtime);

            await client.SendToNode(
                    "entry",
                    RoutingId.From("target-node"),
                    new ProbeRouteMessage("send"))
                .Metadata("trace-id", "first")
                .Metadata("trace-id", "last")
                .Async();

            Assert.Equal(RoutingId.From("target-node"), node.LastNodeSendTarget);
            Assert.Equal(SendFlags.None, node.LastNodeSendFlags);
            Assert.True(ZLinkMeshMetadataCodec.TryDecode(
                node.LastNodeSendMetadata,
                out var metadata));
            Assert.Equal("last", metadata.Find("trace-id"));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task RouteClient_NodeRequest_Forwards_The_Metadata_Snapshot()
    {
        var node = new CapturingSpotNode
        {
            NodeRequestFailure = new InvalidOperationException("request captured")
        };
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        try
        {
            var client = new ZLinkRouteClient(runtime);

            var failure = await Assert.ThrowsAsync<InvalidOperationException>(() =>
                client.RequestToNode(
                        "entry",
                        RoutingId.From("target-node"),
                        new ProbeRouteMessage("request"))
                    .Metadata("trace-id", "request")
                    .Async<ProbeReply>()
                    .AsTask());

            Assert.Equal("request captured", failure.Message);
            Assert.Equal(RoutingId.From("target-node"), node.LastNodeRequestTarget);
            Assert.Equal(SendFlags.None, node.LastNodeRequestFlags);
            Assert.True(ZLinkMeshMetadataCodec.TryDecode(
                node.LastNodeRequestMetadata,
                out var metadata));
            Assert.Equal("request", metadata.Find("trace-id"));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Actor_Creation_Uses_The_FrameworkIssued_EntrySpot_Identity()
    {
        var services = new ServiceCollection()
            .AddScoped<CreationProbeActorFactory>()
            .BuildServiceProvider();
        var node = new CapturingSpotNode();
        var registration = new ZLinkFrameworkRegistration
        {
            ImplicitHandlerAutoRegistrationEnabled = false
        };
        registration.SpotNodes["entry"] = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "entry",
            RoutingId = RoutingId.From("entry-node"),
            Router = new ZLinkSpotRouterCapabilityRegistration
            {
                BindEndpoint = "inproc://entry-actor"
            },
            EntrySpotType = typeof(ProbeEntrySpot),
            ActorFactories =
            {
                ["probe"] = typeof(CreationProbeActorFactory)
            }
        };
        registration.ActorCatalog.Build(registration.SpotNodes.Values);
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new CapturingBackendAdapterFactory(node),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));
        await runtime.StartAsync(CancellationToken.None);
        try
        {
            var created = await runtime.CreateActorAsync("actor-entry-rid", "probe");
            Assert.True(runtime.TryGetCreatedActorState("actor-entry-rid", out var state));
            var context = created.Actor.Context;
            var backendActor = Assert.Single(node.CreatedActors);

            Assert.Equal("actor-entry-rid", context.ActorId);
            Assert.Equal(backendActor.Generation, context.ObjectGeneration);
            Assert.Equal("entry", context.MeshName);
            Assert.Same(context, created.Actor.Context);
            Assert.Equal(node.EntryRoutingId, Assert.Single(node.CreatedActorEntryRids));

            var sourceSpotId = context.SpotId;
            state.InvalidateContext();

            // Owner cutover fences operations but leaves source callback
            // identity readable until the source instance is released.
            Assert.Equal("actor-entry-rid", context.ActorId);
            Assert.Equal(backendActor.Generation, context.ObjectGeneration);
            Assert.Equal(sourceSpotId, context.SpotId);
            var stale = Assert.Throws<ZLinkFrameworkException>(
                () => context.JoinEntrySpot());
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, stale.Kind);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task CancelledCreateWaiter_DoesNotDetachTheSharedCreationTransaction()
    {
        var probe = new ControlledCreationProbe();
        var services = new ServiceCollection()
            .AddSingleton(probe)
            .AddScoped<ControlledCreationProbeActorFactory>()
            .BuildServiceProvider();
        var node = new CapturingSpotNode();
        node.SetRoutingId(RoutingId.From("actor-node"));
        var registration = new ZLinkFrameworkRegistration();
        registration.SpotNodes["actor-node"] = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "actor-node",
            ActorFactories = { ["controlled"] = typeof(ControlledCreationProbeActorFactory) }
        };
        registration.ActorCatalog.Build(registration.SpotNodes.Values);
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new CapturingBackendAdapterFactory(node),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(services.GetRequiredService<IServiceScopeFactory>(), registration));
        var sessions = new ZLinkActorSessionManager(
            runtime, services, () => node, null, new ZLinkBoundSessionService(runtime));
        using var cancelledWaiter = new CancellationTokenSource();

        var first = sessions.CreateAndBindActorAsync(
                "actor-create-shared",
                "controlled",
                cancelledWaiter.Token)
            .AsTask();
        await probe.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        cancelledWaiter.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => first);

        var second = sessions.CreateAndBindActorAsync(
                "actor-create-shared",
                "controlled",
                CancellationToken.None)
            .AsTask();
        Assert.False(second.IsCompleted);
        probe.Release.TrySetResult();

        var result = await second.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal("actor-create-shared", result.Actor.Context.ActorId);
        Assert.Single(node.CreatedActors);
        Assert.True(sessions.TryGetCreatedActorState("actor-create-shared", out var state));
        Assert.Same(result.Actor, state.Actor);
    }

    [Fact]
    public async Task ActorActivation_UsesNodeWideConcurrencyAdmission()
    {
        var probe = new ControlledCreationProbe();
        var services = new ServiceCollection()
            .AddSingleton(probe)
            .AddScoped<ControlledCreationProbeActorFactory>()
            .BuildServiceProvider();
        var node = new CapturingSpotNode();
        node.SetRoutingId(RoutingId.From("actor-node"));
        var registration = new ZLinkFrameworkRegistration();
        registration.SpotNodes["actor-node"] = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "actor-node",
            ActorFactories = { ["controlled"] = typeof(ControlledCreationProbeActorFactory) }
        };
        registration.ActorCatalog.Build(registration.SpotNodes.Values);
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new CapturingBackendAdapterFactory(node),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var admission = new ZLinkActivationConcurrencyAdmission(1);
        var sessions = new ZLinkActorSessionManager(
            runtime,
            services,
            () => node,
            null,
            new ZLinkBoundSessionService(runtime),
            _ => admission);

        var first = sessions.CreateAndBindActorAsync(
                "actor-concurrency-first",
                "controlled")
            .AsTask();
        await probe.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var rejected = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await sessions.CreateAndBindActorAsync(
                "actor-concurrency-second",
                "controlled"));
        Assert.Equal(ZLinkFrameworkErrorKind.CapacityExceeded, rejected.Kind);
        Assert.Equal(1, admission.Active);

        probe.Release.TrySetResult();
        var firstResult = await first.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal("actor-concurrency-first", firstResult.Actor.Context.ActorId);
        Assert.Equal(0, admission.Active);
    }

    [Fact]
    public async Task ActorCreation_PublishFailure_CompensatesClaimNativeActorAndRuntimeState()
    {
        var services = new ServiceCollection()
            .AddScoped<CreationProbeActorFactory>()
            .BuildServiceProvider();
        var node = new CapturingSpotNode();
        node.SetRoutingId(RoutingId.From("actor-node"));
        var registration = new ZLinkFrameworkRegistration
        {
            DefaultRequestTimeout = TimeSpan.FromSeconds(1)
        };
        registration.SpotNodes["actor-node"] = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "actor-node",
            ActorFactories = { ["probe"] = typeof(CreationProbeActorFactory) }
        };
        registration.ActorCatalog.Build(registration.SpotNodes.Values);
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new CapturingBackendAdapterFactory(node),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(services.GetRequiredService<IServiceScopeFactory>(), registration));
        var lifecycle = new FailingPublishActorLifecycle();
        var teardownEvents = new List<string>();
        node.BeforeDestroy = _ => teardownEvents.Add("native-destroy");
        lifecycle.BeforeRelease = () => teardownEvents.Add("ownership-release");
        var state = new ZLinkActorRuntimeState("actor-create-fail");
        ZLinkActorContext EnsureContext() => state.GetOrCreateContext(
            () => new ZLinkActorContext(
                runtime,
                state,
                "mesh",
                state.NativeActorRef?.Generation ?? 1UL,
                state.SpotId,
                new ZLinkBoundSessionService(runtime)));
        var coordinator = new ZLinkActorCreationCoordinator(
            runtime,
            services,
            () => node,
            lifecycle,
            _ => EnsureContext(),
            (actor, actorState) =>
            {
                actorState.BindActorInstance(actor);
                var context = EnsureContext();
                Assert.Same(context, actor.Context);
                if (actorState.TryBeginActorConfiguration()) actor.Configure();
                return context;
            },
            async (actorState, nativeActor, cancellationToken) =>
            {
                await node.DestroyActorAsync(
                    nativeActor,
                    registration.DefaultRequestTimeout,
                    cancellationToken);
                await lifecycle.ReleaseActorAsync(actorState.RuntimeActorId, cancellationToken);
                await actorState.ExecuteLockedAsync(
                    () => actorState.ClearAfterDestroyOnLane(),
                    CancellationToken.None);
            });

        var failure = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await coordinator.CreateAndBindActorAsync(
                state,
                state.ActorId,
                "probe",
                ZLinkMessage.Empty,
                failIfExists: true,
                ZLinkActorClaimMode.NewOwner,
                CancellationToken.None));

        Assert.Equal("publish failed", failure.Message);
        Assert.Equal(1, lifecycle.ReleaseCalls);
        Assert.Single(node.DestroyedActors);
        Assert.Equal(["native-destroy", "ownership-release"], teardownEvents);
        Assert.Null(state.Actor);
        Assert.Null(state.ActorType);
        Assert.Null(state.NativeActorRef);
    }

    [Fact]
    public async Task ActorDestroy_NativeNotFound_IsTerminalAndThenReleasesOwnership()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var locationRuntime = new ZLinkLocationRuntime(
            options, store, time);
        Assert.True(await locationRuntime.RenewOwnerLeaseOnceAsync());
        var resolvers = new ZLinkStoreLocationResolvers(
            store,
            new ZLinkOwnerLeaseTracker(store, options, time),
            new ZLinkObservedLocationGenerations());
        await using var lifecycle = new ZLinkLocationLifecycle(locationRuntime, resolvers);
        await CreateTrackedActorOwnershipAsync(
            store,
            locationRuntime.OwnerId,
            lifecycle.ActorOwnership,
            "probe",
            "actor-destroy-not-found",
            RoutingId.From("actor-node"),
            CancellationToken.None);

        var services = new ServiceCollection().BuildServiceProvider();
        var node = new CapturingSpotNode
        {
            DestroyFailure = new ZlinkRequestException(ZlinkRequestException.ErrorCode.NotFound)
        };
        node.SetRoutingId(RoutingId.From("actor-node"));
        var registration = new ZLinkFrameworkRegistration
        {
            DefaultRequestTimeout = TimeSpan.FromSeconds(1)
        };
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new CapturingBackendAdapterFactory(node),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(services.GetRequiredService<IServiceScopeFactory>(), registration));
        var sessions = new ZLinkActorSessionManager(
            runtime, services, () => node, lifecycle, new ZLinkBoundSessionService(runtime));
        var state = sessions.GetOrCreateState("actor-destroy-not-found");
        var context = state.GetOrCreateContext(
            () => new ZLinkActorContext(
                runtime,
                state,
                "mesh",
                state.NativeActorRef?.Generation ?? 1UL,
                state.SpotId,
                new ZLinkBoundSessionService(runtime)));
        var actor = new CreationProbeActor(context);
        state.BindActorInstance(actor);
        state.BindNativeActorRef(new ZLinkBackendActorRef(node.RoutingId, state.ActorId, 1));

        await sessions.DestroyActorAsync(node.RoutingId, actor);

        Assert.False(sessions.TryGetCreatedActorState(state.ActorId, out _));
        Assert.Single(node.DestroyedActors);
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await AuthorityLocationTestFixture.ReadActorAsync(store, state.ActorId));
    }

    [Fact]
    public async Task ActorDestroy_NativeFailure_RetainsRefAndOwnershipUntilRetry()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var locationRuntime = new ZLinkLocationRuntime(
            options, store, time);
        Assert.True(await locationRuntime.RenewOwnerLeaseOnceAsync());
        var resolvers = new ZLinkStoreLocationResolvers(
            store,
            new ZLinkOwnerLeaseTracker(store, options, time),
            new ZLinkObservedLocationGenerations());
        await using var lifecycle = new ZLinkLocationLifecycle(locationRuntime, resolvers);
        await CreateTrackedActorOwnershipAsync(
            store,
            locationRuntime.OwnerId,
            lifecycle.ActorOwnership,
            "probe",
            "actor-destroy-native-retry",
            RoutingId.From("actor-node"),
            CancellationToken.None);

        var services = new ServiceCollection().BuildServiceProvider();
        var node = new CapturingSpotNode
        {
            DestroyFailure = new InvalidOperationException("native destroy failed")
        };
        node.SetRoutingId(RoutingId.From("actor-node"));
        var registration = new ZLinkFrameworkRegistration
        {
            DefaultRequestTimeout = TimeSpan.FromSeconds(1)
        };
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new CapturingBackendAdapterFactory(node),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(services.GetRequiredService<IServiceScopeFactory>(), registration));
        var sessions = new ZLinkActorSessionManager(
            runtime, services, () => node, lifecycle, new ZLinkBoundSessionService(runtime));
        var state = sessions.GetOrCreateState("actor-destroy-native-retry");
        var context = state.GetOrCreateContext(
            () => new ZLinkActorContext(
                runtime,
                state,
                "mesh",
                state.NativeActorRef?.Generation ?? 1UL,
                state.SpotId,
                new ZLinkBoundSessionService(runtime)));
        var actor = new CreationProbeActor(context);
        var nativeActor = new ZLinkBackendActorRef(node.RoutingId, state.ActorId, 1);
        state.BindActorInstance(actor);
        state.BindNativeActorRef(nativeActor);

        var failure = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await sessions.DestroyActorAsync(node.RoutingId, actor));

        Assert.Equal("native destroy failed", failure.Message);
        Assert.Equal(nativeActor, state.NativeActorRef);
        Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await AuthorityLocationTestFixture.ReadActorAsync(store, state.ActorId));
        Assert.Null(await sessions.FindActorAsync(state.ActorId));
        await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await state.GetOrStartActorCreationAsync(
                "probe",
                false,
                () => Task.FromResult<IZLinkActor>(actor),
                CancellationToken.None));
        Assert.Throws<ZLinkFrameworkException>(() => state.BindActorInstance(actor));
        Assert.Throws<ZLinkFrameworkException>(() => state.BindSession(
            node.RoutingId,
            RoutingId.From("session"),
            "binding"));
        await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await state.ExecuteDispatchAsync(
                CreateHeader("blocked"),
                _ => ValueTask.CompletedTask,
                CancellationToken.None));

        node.DestroyFailure = null;
        await sessions.DestroyActorAsync(node.RoutingId, actor);

        Assert.Equal(2, node.DestroyedActors.Count);
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await AuthorityLocationTestFixture.ReadActorAsync(store, state.ActorId));
    }

    [Fact]
    public async Task OwnershipLoss_NativeFailure_KeepsQuarantinedStateUntilReconciliationCompletes()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var locationRuntime = new ZLinkLocationRuntime(
            options, store, time);
        Assert.True(await locationRuntime.RenewOwnerLeaseOnceAsync());
        var resolvers = new ZLinkStoreLocationResolvers(
            store,
            new ZLinkOwnerLeaseTracker(store, options, time),
            new ZLinkObservedLocationGenerations());
        await using var lifecycle = new ZLinkLocationLifecycle(locationRuntime, resolvers);
        await CreateTrackedActorOwnershipAsync(
            store,
            locationRuntime.OwnerId,
            lifecycle.ActorOwnership,
            "probe",
            "actor-ownership-loss-retry",
            RoutingId.From("actor-node"),
            CancellationToken.None);

        var retryStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var allowRetry = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var destroyAttempts = 0;
        var node = new CapturingSpotNode
        {
            DestroyHandler = async (_, cancellationToken) =>
            {
                if (Interlocked.Increment(ref destroyAttempts) == 1)
                    throw new InvalidOperationException("native destroy failed");

                retryStarted.TrySetResult();
                await allowRetry.Task.WaitAsync(cancellationToken);
            }
        };
        node.SetRoutingId(RoutingId.From("actor-node"));
        var services = new ServiceCollection().BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration
        {
            DefaultRequestTimeout = TimeSpan.FromSeconds(1)
        };
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new CapturingBackendAdapterFactory(node),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(services.GetRequiredService<IServiceScopeFactory>(), registration));
        var sessions = new ZLinkActorSessionManager(
            runtime, services, () => node, lifecycle, new ZLinkBoundSessionService(runtime));
        var state = sessions.GetOrCreateState("actor-ownership-loss-retry");
        var context = state.GetOrCreateContext(
            () => new ZLinkActorContext(
                runtime,
                state,
                "mesh",
                state.NativeActorRef?.Generation ?? 1UL,
                state.SpotId,
                new ZLinkBoundSessionService(runtime)));
        var actor = new CreationProbeActor(context);
        var nativeActor = new ZLinkBackendActorRef(node.RoutingId, state.ActorId, 1);
        state.BindActorInstance(actor);
        state.BindNativeActorRef(nativeActor);

        await sessions.DeactivateActorOnOwnershipLossAsync(state.ActorId);
        var retry = sessions.DeactivateActorOnOwnershipLossAsync(state.ActorId).AsTask();
        await retryStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(nativeActor, state.NativeActorRef);
        Assert.True(state.IsTeardownPending);
        Assert.Null(await sessions.FindActorAsync(state.ActorId));
        Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await AuthorityLocationTestFixture.ReadActorAsync(store, state.ActorId));

        allowRetry.TrySetResult();
        await retry.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.False(state.IsTeardownPending);
        Assert.Null(state.NativeActorRef);
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await AuthorityLocationTestFixture.ReadActorAsync(store, state.ActorId));
        Assert.Equal(2, node.DestroyedActors.Count);
    }

    [Fact]
    public async Task ActorDestroy_ReleaseFailure_RetainsStateForTheNextOwnedRetry()
    {
        var time = new ManualTimeProvider();
        var store = new ZLinkInMemoryLocationStore(time);
        var actorStore = new FailOnceRemoveActorStore(store);
        var options = new ZLinkLocationOptions { PollingInterval = TimeSpan.Zero };
        var locationRuntime = new ZLinkLocationRuntime(
            options, actorStore, time);
        Assert.True(await locationRuntime.RenewOwnerLeaseOnceAsync());
        var resolvers = new ZLinkStoreLocationResolvers(
            store,
            new ZLinkOwnerLeaseTracker(store, options, time),
            new ZLinkObservedLocationGenerations());
        await using var lifecycle = new ZLinkLocationLifecycle(locationRuntime, resolvers);
        await CreateTrackedActorOwnershipAsync(
            store,
            locationRuntime.OwnerId,
            lifecycle.ActorOwnership,
            "probe",
            "actor-destroy-retry",
            RoutingId.From("actor-node"),
            CancellationToken.None);

        var services = new ServiceCollection().BuildServiceProvider();
        var node = new CapturingSpotNode();
        node.SetRoutingId(RoutingId.From("actor-node"));
        var registration = new ZLinkFrameworkRegistration
        {
            DefaultRequestTimeout = TimeSpan.FromSeconds(1)
        };
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new CapturingBackendAdapterFactory(node),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(services.GetRequiredService<IServiceScopeFactory>(), registration));
        var sessions = new ZLinkActorSessionManager(
            runtime, services, () => node, lifecycle, new ZLinkBoundSessionService(runtime));
        var state = sessions.GetOrCreateState("actor-destroy-retry");
        var context = state.GetOrCreateContext(
            () => new ZLinkActorContext(
                runtime,
                state,
                "mesh",
                state.NativeActorRef?.Generation ?? 1UL,
                state.SpotId,
                new ZLinkBoundSessionService(runtime)));
        var actor = new CreationProbeActor(context);
        state.BindActorInstance(actor);
        state.BindNativeActorRef(new ZLinkBackendActorRef(node.RoutingId, state.ActorId, 1));

        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await sessions.DestroyActorAsync(node.RoutingId, actor));

        Assert.False(sessions.TryGetCreatedActorState(state.ActorId, out _));
        Assert.NotNull(state.NativeActorRef);
        Assert.True(state.IsTeardownPending);
        Assert.Single(node.DestroyedActors);

        await sessions.DestroyActorAsync(node.RoutingId, actor);

        Assert.False(sessions.TryGetCreatedActorState(state.ActorId, out _));
        Assert.Single(node.DestroyedActors);
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await AuthorityLocationTestFixture.ReadActorAsync(store, state.ActorId));
    }

    [Fact]
    public async Task ConcurrentActorDestroy_FromCurrentTurn_CallersAwaitOneTeardownTransaction()
    {
        var services = new ServiceCollection().BuildServiceProvider();
        var node = new CapturingSpotNode();
        node.SetRoutingId(RoutingId.From("actor-node"));
        var destroyStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseDestroy = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        node.DestroyHandler = async (_, cancellationToken) =>
        {
            destroyStarted.TrySetResult();
            await releaseDestroy.Task.WaitAsync(cancellationToken);
        };
        var registration = new ZLinkFrameworkRegistration
        {
            DefaultRequestTimeout = TimeSpan.FromSeconds(1)
        };
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new CapturingBackendAdapterFactory(node),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(services.GetRequiredService<IServiceScopeFactory>(), registration));
        var sessions = new ZLinkActorSessionManager(
            runtime, services, () => node, null, new ZLinkBoundSessionService(runtime));
        var state = sessions.GetOrCreateState("actor-destroy-concurrent");
        var context = state.GetOrCreateContext(
            () => new ZLinkActorContext(
                runtime,
                state,
                "mesh",
                state.NativeActorRef?.Generation ?? 1UL,
                state.SpotId,
                new ZLinkBoundSessionService(runtime)));
        var actor = new CreationProbeActor(context);
        state.BindActorInstance(actor);
        state.BindNativeActorRef(new ZLinkBackendActorRef(node.RoutingId, state.ActorId, 1));

        var first = state.ExecuteDispatchAsync(
                CreateHeader("destroy-from-turn"),
                async _ =>
                {
                    await sessions.DestroyActorAsync(node.RoutingId, actor);
                    Assert.False(destroyStarted.Task.IsCompleted);
                },
                CancellationToken.None)
            .AsTask();
        await first.WaitAsync(TimeSpan.FromSeconds(5));
        await destroyStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(state.IsTeardownPending);
        var second = sessions.DestroyActorAsync(node.RoutingId, actor).AsTask();
        releaseDestroy.TrySetResult();

        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Single(node.DestroyedActors);
        Assert.False(state.IsTeardownPending);
    }

    [Fact]
    public async Task ActorDestroy_DuringBoundRequest_IsDeferredUntilReplyFinalization()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        try
        {
            var actorRef = new ZLinkBackendActorRef(
                RoutingId.From("entry-node"),
                "actor-destroy-after-reply",
                1);
            var actor = RegisterProbeActor(runtime, actorRef);
            node.BeforeNoBindReply = _ => node.LifecycleEvents.Enqueue("reply");
            node.BeforeDestroy = _ => node.LifecycleEvents.Enqueue("destroy");
            var parts = CreateActorRequestParts(
                actorRef,
                "destroy-request",
                "destroy",
                requestId: 81,
                flags: 1);

            await DispatchEntryActorPartsAsync(runtime, parts, CancellationToken.None);

            Assert.Single(node.DestroyedActors);
            Assert.Null(await runtime.FindActorAsync(actor.ActorId));
            Assert.Equal(new[] { "reply", "destroy" }, node.LifecycleEvents.ToArray());
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ActorDestroy_DuringBoundRequest_RejectsPreCancelledCall()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        try
        {
            var actorRef = new ZLinkBackendActorRef(
                RoutingId.From("entry-node"),
                "actor-destroy-cancelled",
                1);
            var actor = RegisterProbeActor(runtime, actorRef);
            var activation = runtime.GetSpotNodeRuntime("entry").EntrySpotActivation
                             ?? throw new InvalidOperationException("Entry Spot activation was not created.");
            await using var dispatch = ZLinkBoundSessionDispatchScope.Enter(actor.ActorId);
            using var cancelled = new CancellationTokenSource();
            cancelled.Cancel();

            await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
                await activation.DestroyActorAsync(actor, cancelled.Token));
            await dispatch.DrainAsync(CancellationToken.None);

            Assert.Empty(node.DestroyedActors);
            Assert.Same(actor, await runtime.FindActorAsync(actor.ActorId));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public void MeshRecordAdapter_PreservesDirectRouteIdentityAndFences()
    {
        var sourceNode = RoutingId.From("source-node");
        var actor = new ActorRef("actor-forward", 7, "mesh-a", RoutingId.From("old-owner"));
        var operationId = new MeshOperationId(11, 12);
        using var batch = new MeshReceiveBatch();
        batch.Add(
            new MeshReceiveRecord(
                MeshRecordKind.ActorRequest,
                MeshReadyDomains.Application,
                sourceNode,
                "source-spot",
                1,
                actor,
                operationId,
                MeshOperationKind.ActorRequest,
                channelName: null,
                topic: null,
                applicationMetadata: null,
                partOffset: 0,
                partCount: 2,
                terminalResult: 0,
                failureErrno: 0,
                kindData: null,
                targetNodeGeneration: 21,
                authorityOwnerGeneration: 22,
                ownerLeaseGeneration: 23,
                messageFollowHopCount: 7,
                deadlineUnixMs: ulong.MaxValue),
            [
                Message.From(Encoding.UTF8.GetBytes("header")),
                Message.From(Encoding.UTF8.GetBytes("body"))
            ]);

        var parts = ZLinkMeshRecordAdapters.ToActorParts(
            batch,
            0,
            batch[0],
            requestId: 91);
        try
        {
            Assert.Equal(2, parts.Count);
            Assert.All(parts, part =>
            {
                Assert.Equal(operationId, part.RouteContext.OperationId);
                Assert.Equal((byte)7, part.RouteContext.MessageFollowHopCount);
                Assert.Equal(21ul, part.RouteContext.TargetNodeGeneration);
                Assert.Equal(22ul, part.RouteContext.AuthorityOwnerGeneration);
                Assert.Equal(23ul, part.RouteContext.OwnerLeaseGeneration);
                Assert.Equal(0ul, part.RouteContext.DeadlineUnixMs);
                Assert.Equal(91ul, part.RouteContext.ReplyRequestId);
                Assert.Equal(1u, part.RouteContext.ReplyFlags);
                Assert.Equal(91ul, part.RequestId);
                Assert.Equal(1u, part.Flags);
            });
        }
        finally
        {
            foreach (var part in parts) part.Message.Dispose();
        }
    }

    [Fact]
    public void MeshRecordAdapter_RejectsDeadlineAboveSignedRangeUnlessItIsCoreSentinel()
    {
        var error = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkMeshRecordAdapters.NormalizeDeadline(
                checked((ulong)long.MaxValue + 1)));

        Assert.Equal(
            ZLinkFrameworkErrorKind.ProtocolError,
            error.Kind);
        Assert.Equal(
            0ul,
            ZLinkMeshRecordAdapters.NormalizeDeadline(ulong.MaxValue));
    }

    [Fact]
    public async Task ActorMessageFollowDispatcher_RejectsDirectRouteAtEightHops()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        try
        {
            var state = runtime.GetOrCreateActorState("actor-hop-limit");
            var source = new ZLinkBackendActorRef(
                RoutingId.From("old-owner"), "actor-hop-limit", 3);
            var target = new ZLinkBackendActorRef(
                RoutingId.From("new-owner"), "actor-hop-limit", 4);
            state.Handoff.BeginCapture();
            state.Handoff.CutoverCaptureToMessageFollow(
                committedFrameCount: 0,
                sourceActor: source,
                targetActor: target,
                targetMeshName: "mesh-a",
                sourceNodeGeneration: 31,
                targetNodeGeneration: 32,
                sourceAuthorityOwnerGeneration: 41,
                targetAuthorityOwnerGeneration: 42,
                sourceOwnerLeaseGeneration: 51,
                targetOwnerLeaseGeneration: 52);
            state.Handoff.CommitMessageFollow(TimeSpan.FromSeconds(5));
            using var body = Message.From(Encoding.UTF8.GetBytes("body"));

            var exception = Assert.Throws<ZLinkFrameworkException>(() =>
                ZLinkActorMessageFollowDispatcher.TryFollow(
                    runtime,
                    state,
                    source,
                    RoutingId.From("caller-node"),
                    default,
                    requestId: 61,
                    flags: 1,
                    routeContext: new ZLinkBackendActorRouteContext(
                        new MeshOperationId(71, 72),
                        MessageFollowHopCount: 8,
                        TargetNodeGeneration: 31,
                        AuthorityOwnerGeneration: 41,
                        OwnerLeaseGeneration: 51,
                        ReplyRequestId: 61,
                        ReplyFlags: 1),
                    header: CreateHeader("hop-limit"),
                    body: body));

            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, exception.Kind);
            Assert.Empty(node.MessageFollowParts);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task InvalidMessageFollowFallback_PreservesPumpCaptureOrder()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        try
        {
            var sourceActor = new ZLinkBackendActorRef(
                node.RoutingId,
                "actor-invalid-follow-capture",
                17);
            var targetActor = new ZLinkBackendActorRef(
                RoutingId.From("target-node"),
                sourceActor.ActorId,
                18);
            var actor = RegisterProbeActor(runtime, sourceActor);
            var state = runtime.GetOrCreateActorState(actor.ActorId);
            state.Handoff.BeginCapture();
            state.Handoff.CutoverCaptureToMessageFollow(
                committedFrameCount: 0,
                sourceActor: sourceActor,
                targetActor: targetActor,
                targetMeshName: "mesh-a",
                sourceNodeGeneration: 43,
                targetNodeGeneration: 44,
                sourceAuthorityOwnerGeneration: 47,
                targetAuthorityOwnerGeneration: 48,
                sourceOwnerLeaseGeneration: 53,
                targetOwnerLeaseGeneration: 54);
            state.Handoff.CommitMessageFollow(TimeSpan.FromSeconds(5));
            state.Handoff.CompleteSourceMigration();
            state.Handoff.BeginCapture();

            var source = new ZLinkServiceWireCodec.RequestSourceFence(
                "caller-owner",
                19,
                RoutingId.From("caller-node"),
                23);
            var firstMessages = new[]
            {
                Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(
                    CreateHeader("first")).Span),
                Message.From("first-body")
            };
            var secondMessages = new[]
            {
                Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(
                    CreateHeader("second")).Span),
                Message.From("second-body")
            };
            var parts = CreateStaleManagedParts(
                    sourceActor,
                    source,
                    new MeshOperationId(29, 31),
                    firstMessages,
                    flags: 0,
                    messageFollowHopCount: 8)
                .Concat(CreateStaleManagedParts(
                    sourceActor,
                    source,
                    new MeshOperationId(37, 41),
                    secondMessages,
                    flags: 0,
                    messageFollowHopCount: 8))
                .ToArray();

            using var batch = ZLinkActorHandoffIngress.CaptureMovingFrames(
                runtime,
                parts);

            Assert.Equal(0, batch.Count);
            var captured = state.Handoff.SnapshotFrames();
            Assert.Equal(
                new[] { "first-body", "second-body" },
                captured.Select(static frame =>
                    Encoding.UTF8.GetString(frame.Body)));
            Assert.Equal(
                new long[] { 0, 1 },
                captured
                    .Select(static frame => frame.ArrivalIndex));
            state.Handoff.Reset();
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task StaleManagedIngress_CapturesBeforeMessageFollowAndRestoresAbortOrder()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        try
        {
            var sourceActor = new ZLinkBackendActorRef(
                node.RoutingId,
                "actor-stale-managed-capture",
                17);
            var actor = RegisterProbeActor(runtime, sourceActor);
            var state = runtime.GetOrCreateActorState(actor.ActorId);
            state.Handoff.BeginCapture();
            var source = new ZLinkServiceWireCodec.RequestSourceFence(
                "caller-owner",
                19,
                RoutingId.From("caller-node"),
                23);
            var requestOperation = new MeshOperationId(29, 31);
            var sendOperation = new MeshOperationId(37, 41);
            var requestHeader = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Request,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                new ZlinkStreamRequestSeq(1),
                "capture-request",
                ZlinkStreamMetadata.Empty);
            var sendHeader = CreateHeader("capture-send");
            var requestMessages = new[]
            {
                Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(
                    requestHeader).Span),
                Message.From(Encoding.UTF8.GetBytes("request-body"))
            };
            var sendMessages = new[]
            {
                Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(
                    sendHeader).Span),
                Message.From(Encoding.UTF8.GetBytes("send-body"))
            };
            var directReplyCount = 0;

            Assert.True(
                ZLinkActorHandoffIngress
                    .TryCaptureOrFollowStaleManagedIngress(
                        runtime,
                        CreateStaleManagedParts(
                            sourceActor,
                            source,
                            requestOperation,
                            requestMessages,
                            flags: 1,
                            directReply: (_, _) =>
                            {
                                Interlocked.Increment(ref directReplyCount);
                                return SubmitResult.Ok;
                            })));
            Assert.True(
                ZLinkActorHandoffIngress
                    .TryCaptureOrFollowStaleManagedIngress(
                        runtime,
                        CreateStaleManagedParts(
                            sourceActor,
                            source,
                            sendOperation,
                            sendMessages,
                            flags: 0)));

            state.Handoff.SealCapture();
            var committed = state.Handoff.FreezeCaptureCommitBoundary();
            Assert.Equal(2, committed.Frames.Count);
            var request = committed.Frames[0];
            var send = committed.Frames[1];
            Assert.Equal(0, request.ArrivalIndex);
            Assert.Equal(1, send.ArrivalIndex);
            Assert.Equal(requestOperation, request.RouteContext.OperationId);
            Assert.Equal(
                requestOperation.Low,
                request.RelocationReplyRouteId);
            Assert.False(
                string.IsNullOrWhiteSpace(
                    request.RouteContext.ReplyCapability));
            Assert.Equal(source, request.RequestSource);
            Assert.Equal(sendOperation, send.RouteContext.OperationId);
            Assert.Equal(0UL, send.RelocationReplyRouteId);
            Assert.Equal(0, Volatile.Read(ref directReplyCount));
            Assert.All(
                requestMessages.Concat(sendMessages),
                message => Assert.Throws<ObjectDisposedException>(
                    () => message.AsReadOnlySpan()));

            var restored = state.Handoff.AbortCapture();
            Assert.Equal(
                new long[] { 0, 1 },
                restored.Select(static frame => frame.ArrivalIndex));
            Assert.Equal(
                new[] { "request-body", "send-body" },
                restored.Select(static frame =>
                    Encoding.UTF8.GetString(frame.Body)));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public void ActorMessageFollowerToRelay_PreservesExactDirectRouteAndPayload()
    {
        var localNode = new CapturingSpotNode();
        var states = new Dictionary<string, ZLinkActorRuntimeState>(StringComparer.Ordinal);
        var coordinator = new ZLinkActorBoundSessionCoordinator(
            actorId => states.TryGetValue(actorId, out var state)
                ? state
                : states[actorId] = new ZLinkActorRuntimeState(actorId),
            () => localNode,
            _ => localNode,
            new ZLinkFrameworkRegistration(),
            () => CancellationToken.None);
        (ZLinkBackendActorRouteContext Route, ulong SourceGeneration,
            ZLinkServiceWireCodec.RequestSourceFence? Source,
            byte[] Header, byte[] Body)? relayed = null;
        coordinator.RemoteFrameRelay = (
            _, _, _, _, _, _, _, route, sourceGeneration, source, _,
            header, body) =>
        {
            relayed = (route, sourceGeneration, source, header, body);
            return true;
        };
        var incomingRoute = new ZLinkBackendActorRouteContext(
            new MeshOperationId(81, 82),
            MessageFollowHopCount: 2,
            TargetNodeGeneration: 73,
            AuthorityOwnerGeneration: 74,
            OwnerLeaseGeneration: 75,
            ReplyRequestId: 86,
            ReplyFlags: 87);
        var target = new ZLinkBackendActorRef(
            RoutingId.From("remote-owner"), "actor-relay", 9);
        var messageFollowLease = new ZLinkActorMessageFollowLease(TimeProvider.System);
        messageFollowLease.Commit(TimeSpan.FromSeconds(5));
        var messageFollowRoute = new ZLinkActorMessageFollowRoute(
            new ZLinkBackendActorRef(
                RoutingId.From("old-owner"), "actor-relay", 8),
            target,
            "mesh-a",
            SourceNodeGeneration: 73,
            TargetNodeGeneration: 83,
            SourceAuthorityOwnerGeneration: 74,
            TargetAuthorityOwnerGeneration: 84,
            SourceOwnerLeaseGeneration: 75,
            TargetOwnerLeaseGeneration: 85,
            Lease: messageFollowLease);
        var route = ZLinkActorMessageFollower.AdvanceRoute(
            messageFollowRoute,
            incomingRoute,
            requestId: 86,
            flags: 87);
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "caller-owner",
            91,
            RoutingId.From("caller-node"),
            92);
        using var header = Message.From(Encoding.UTF8.GetBytes("header"));
        using var body = Message.From(Encoding.UTF8.GetBytes("body"));

        Assert.True(coordinator.ForwardPart(
            target,
            RoutingId.From("caller-node"),
            default,
            header,
            hasMore: true,
            flags: SendFlags.DontWait,
            meshName: "mesh-a",
            selectedNode: localNode,
            targetNodeGeneration: 83,
            authorityOwnerGeneration: 84,
            ownerLeaseGeneration: 85,
            routeContext: route,
            sourceNodeGeneration: requestSource.NodeGeneration,
            requestSource: requestSource));
        Assert.True(coordinator.ForwardPart(
            target,
            RoutingId.From("caller-node"),
            default,
            body,
            hasMore: false,
            flags: SendFlags.DontWait,
            meshName: "mesh-a",
            selectedNode: localNode,
            targetNodeGeneration: 83,
            authorityOwnerGeneration: 84,
            ownerLeaseGeneration: 85,
            routeContext: route,
            sourceNodeGeneration: requestSource.NodeGeneration,
            requestSource: requestSource));

        Assert.NotNull(relayed);
        Assert.Equal(route, relayed.Value.Route);
        Assert.Equal(incomingRoute.OperationId, relayed.Value.Route.OperationId);
        Assert.Equal((byte)3, relayed.Value.Route.MessageFollowHopCount);
        Assert.Equal(83ul, relayed.Value.Route.TargetNodeGeneration);
        Assert.Equal(84ul, relayed.Value.Route.AuthorityOwnerGeneration);
        Assert.Equal(85ul, relayed.Value.Route.OwnerLeaseGeneration);
        Assert.Equal(86ul, relayed.Value.Route.ReplyRequestId);
        Assert.Equal(87u, relayed.Value.Route.ReplyFlags);
        Assert.Equal(requestSource.NodeGeneration, relayed.Value.SourceGeneration);
        Assert.Equal(requestSource, relayed.Value.Source);
        Assert.Equal("header", Encoding.UTF8.GetString(relayed.Value.Header));
        Assert.Equal("body", Encoding.UTF8.GetString(relayed.Value.Body));
    }

    [Fact]
    public void MigratedActorSession_RetainsFenceAfterSourceCleanupRegistryRetires()
    {
        var localNode = new CapturingSpotNode();
        var states = new Dictionary<string, ZLinkActorRuntimeState>(StringComparer.Ordinal);
        var coordinator = new ZLinkActorBoundSessionCoordinator(
            actorId => states.TryGetValue(actorId, out var state)
                ? state
                : states[actorId] = new ZLinkActorRuntimeState(actorId),
            () => localNode,
            _ => localNode,
            new ZLinkFrameworkRegistration(),
            () => CancellationToken.None);
        var actorId = "actor-migrated-session-fence";
        var sessionRid = RoutingId.From("session-migrated");
        var bindingToken = ZLinkActorBoundSessionBindingToken.Native(sessionRid);

        coordinator.BindActorSession(
            actorId,
            RoutingId.From("session-node"),
            sessionRid,
            bindingToken,
            bindingGeneration: 4,
            objectGeneration: 7,
            authorityOwnerGeneration: 11,
            meshName: "mesh-a",
            targetNodeGeneration: 13,
            ownerLeaseGeneration: 17,
            sessionOwnerNodeGeneration: 19);

        coordinator.RetireMigratedActorSession(actorId, bindingToken);
        coordinator.CleanupActorSessionsForSession(sessionRid);

        Assert.True(coordinator.TryGetActorBoundSession(actorId, out var retained));
        Assert.Equal(bindingToken, retained.BindingToken);
        Assert.Equal(7UL, retained.ObjectGeneration);
        Assert.Equal(11UL, retained.AuthorityOwnerGeneration);
        Assert.Equal(17UL, retained.OwnerLeaseGeneration);
    }

    [Fact]
    public void ActorMessageFollowerToRelay_RejectsHopLimitWithoutOverflow()
    {
        var lease = new ZLinkActorMessageFollowLease(TimeProvider.System);
        lease.Commit(TimeSpan.FromSeconds(5));
        var route = new ZLinkActorMessageFollowRoute(
            new ZLinkBackendActorRef(
                RoutingId.From("old-owner"), "actor-relay", 8),
            new ZLinkBackendActorRef(
                RoutingId.From("new-owner"), "actor-relay", 8),
            "mesh-a",
            SourceNodeGeneration: 73,
            TargetNodeGeneration: 83,
            SourceAuthorityOwnerGeneration: 74,
            TargetAuthorityOwnerGeneration: 84,
            SourceOwnerLeaseGeneration: 75,
            TargetOwnerLeaseGeneration: 85,
            Lease: lease);
        var incoming = new ZLinkBackendActorRouteContext(
            new MeshOperationId(81, 82),
            MessageFollowHopCount: 8,
            TargetNodeGeneration: 73,
            AuthorityOwnerGeneration: 74,
            OwnerLeaseGeneration: 75);

        var exception = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkActorMessageFollower.AdvanceRoute(
                route,
                incoming,
                requestId: 86,
                flags: 87));

        Assert.Equal(
            ZLinkFrameworkErrorKind.Unavailable,
            exception.Kind);
    }

    [Fact]
    public void ActorMessageFollower_PreservesBoundOneWayOperationAndFence()
    {
        var lease = new ZLinkActorMessageFollowLease(TimeProvider.System);
        lease.Commit(TimeSpan.FromSeconds(5));
        var route = new ZLinkActorMessageFollowRoute(
            new ZLinkBackendActorRef(
                RoutingId.From("old-owner"), "actor-bound-follow", 8),
            new ZLinkBackendActorRef(
                RoutingId.From("new-owner"), "actor-bound-follow", 8),
            "mesh-a",
            SourceNodeGeneration: 73,
            TargetNodeGeneration: 83,
            SourceAuthorityOwnerGeneration: 74,
            TargetAuthorityOwnerGeneration: 84,
            SourceOwnerLeaseGeneration: 75,
            TargetOwnerLeaseGeneration: 85,
            Lease: lease);
        var operation = new MeshOperationId(81, 82);
        var incoming = new ZLinkBackendActorRouteContext(
            operation,
            MessageFollowHopCount: 0,
            TargetNodeGeneration: 73,
            AuthorityOwnerGeneration: 74,
            OwnerLeaseGeneration: 75,
            IsBoundSessionRoute: true);

        var advanced = ZLinkActorMessageFollower.AdvanceRoute(
            route,
            incoming,
            requestId: 0,
            flags: 0);

        Assert.Equal(operation, advanced.OperationId);
        Assert.True(advanced.IsBoundSessionRoute);
        Assert.False(advanced.IsDirectRoute);
        Assert.Equal((ulong)83, advanced.TargetNodeGeneration);
        Assert.Equal((ulong)84, advanced.AuthorityOwnerGeneration);
        Assert.Equal((ulong)85, advanced.OwnerLeaseGeneration);
        Assert.Equal((ulong)0, advanced.ReplyRequestId);
    }

    [Fact]
    public void RemoteActorFrameSource_RequiresExactFence_AndNeverUsesOperationIdAsGeneration()
    {
        var sourceNode = RoutingId.From("caller-node");
        var route = new ZLinkBackendActorRouteContext(
            new MeshOperationId(92, 93),
            MessageFollowHopCount: 1,
            TargetNodeGeneration: 11,
            AuthorityOwnerGeneration: 12,
            OwnerLeaseGeneration: 13);

        var missing = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkFrameworkRuntime.ValidateRemoteActorFrameSource(
                "actor-source-fence",
                route,
                sourceNode,
                sourceNodeGeneration: 0,
                requestSource: null));
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, missing.Kind);

        var mismatched = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkFrameworkRuntime.ValidateRemoteActorFrameSource(
                "actor-source-fence",
                route,
                sourceNode,
                sourceNodeGeneration: 94,
                new ZLinkServiceWireCodec.RequestSourceFence(
                    "caller-owner",
                    95,
                    sourceNode,
                    NodeGeneration: 96)));
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, mismatched.Kind);

        var exact = new ZLinkServiceWireCodec.RequestSourceFence(
            "caller-owner",
            95,
            sourceNode,
            NodeGeneration: 94);
        ZLinkFrameworkRuntime.ValidateRemoteActorFrameSource(
            "actor-source-fence",
            route,
            sourceNode,
            exact.NodeGeneration,
            exact);
    }

    [Fact]
    public async Task RemoteSessionBindPublication_IgnoresOwnerLeaseChange()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            RegisterProbeActor(runtime, actorRef);
            const ulong authorityGeneration = 41;
            node.SetLocalActorAuthorityFence(actorRef, authorityGeneration, 51);
            node.AfterLocalActorAuthorityRead = (read, actor) =>
            {
                if (read == 1)
                    node.SetLocalActorAuthorityFence(actor, authorityGeneration, 52);
            };
            var sessionNode = RoutingId.From("session-node-bind-publication");
            node.AdmittedMeshPeers.Add(new MeshNodePeer(
                1, MeshPeerSource.Discovery, MeshPeerState.Admitted,
                sessionNode, 61, 1, "inproc://session-node-bind-publication",
                1, 0, 1));
            var request = new ZLinkRemoteSessionBindRequest(
                actorRef.ActorId,
                actorRef.NodeRid.ToBytes().ToArray(),
                sessionNode.ToBytes().ToArray(),
                RoutingId.From("session-bind-publication").ToBytes().ToArray(),
                "binding-token",
                1,
                actorRef.Generation,
                "entry",
                61,
                0,
                SessionOwnerId: "session-owner",
                SessionOwnerLeaseGeneration: 71);

            var response = await runtime.BindRemoteBoundSessionRouteAsync(
                request, sessionNode, CancellationToken.None);

            Assert.True(response.Acknowledged);
            Assert.Equal(authorityGeneration, response.AuthorityOwnerGeneration);
            Assert.Equal(51UL, response.OwnerLeaseGeneration);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task RemoteSessionBindPublication_StillRejectsAuthorityChange()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            RegisterProbeActor(runtime, actorRef);
            node.SetLocalActorAuthorityFence(actorRef, 41, 51);
            node.AfterLocalActorAuthorityRead = (read, actor) =>
            {
                if (read == 1)
                    node.SetLocalActorAuthorityFence(actor, 42, 52);
            };
            var sessionNode = RoutingId.From("session-node-bind-authority");
            node.AdmittedMeshPeers.Add(new MeshNodePeer(
                1, MeshPeerSource.Discovery, MeshPeerState.Admitted,
                sessionNode, 61, 1, "inproc://session-node-bind-authority",
                1, 0, 1));
            var request = new ZLinkRemoteSessionBindRequest(
                actorRef.ActorId,
                actorRef.NodeRid.ToBytes().ToArray(),
                sessionNode.ToBytes().ToArray(),
                RoutingId.From("session-bind-authority").ToBytes().ToArray(),
                "binding-token",
                1,
                actorRef.Generation,
                "entry",
                61,
                0,
                SessionOwnerId: "session-owner",
                SessionOwnerLeaseGeneration: 71);

            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                runtime.BindRemoteBoundSessionRouteAsync(
                    request, sessionNode, CancellationToken.None).AsTask());

            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task MovingActorRemoteDirectRequest_PreservesAuthenticatedSourceFenceDuringCapture()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            RegisterProbeActor(runtime, actorRef);
            var state = runtime.GetOrCreateActorState(actorRef.ActorId);
            state.Handoff.BeginCapture();
            var relayNode = RoutingId.From("relay-source-node");
            const ulong relayGeneration = 61;
            node.AdmittedMeshPeers.Add(new MeshNodePeer(
                ConnectionIntentId: 1,
                MeshPeerSource.Discovery,
                MeshPeerState.Admitted,
                relayNode,
                relayGeneration,
                DescriptorRevision: 1,
                Endpoint: "inproc://relay-source-node",
                ChannelCount: 1,
                LastError: 0,
                LastChangedMs: 1));
            const ulong authorityGeneration = 41;
            const ulong ownerLeaseGeneration = 51;
            node.SetLocalActorAuthorityFence(
                actorRef,
                authorityGeneration,
                ownerLeaseGeneration);
            var originalSource = new ZLinkServiceWireCodec.RequestSourceFence(
                "caller-owner",
                71,
                RoutingId.From("caller-node"),
                72);
            var header = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Request,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                new ZlinkStreamRequestSeq(73),
                "request",
                ZlinkStreamMetadata.Empty);

            await runtime.DispatchRemoteActorFrameAsync(
                actorRef.ActorId,
                actorRef.Generation + 1,
                actorRef.NodeRid,
                targetNodeGeneration: 1,
                authorityGeneration,
                ownerLeaseGeneration,
                authenticatedRelayNodeRid: relayNode,
                relayNodeRid: relayNode,
                relayGeneration,
                originalSource.NodeRid,
                originalSource.NodeGeneration,
                sourceSessionRid: default,
                originalSource,
                Array.Empty<byte>(),
                new MeshOperationId(81, 82),
                messageFollowHopCount: 1,
                replyRequestId: 83,
                replyFlags: 1,
                replyCapability: "reply-capability",
                deadlineUnixMs: ulong.MaxValue,
                ZLinkStreamProtocolDefaults.EncodeHeader(header).ToArray(),
                Encoding.UTF8.GetBytes("payload"),
                CancellationToken.None);

            var captured = Assert.Single(state.Handoff.SnapshotFrames());
            Assert.Equal(new MeshOperationId(81, 82), captured.RouteContext.OperationId);
            Assert.Equal(originalSource.NodeGeneration, captured.SourceNodeGeneration);
            Assert.Equal(originalSource, captured.RequestSource);
            Assert.Equal(0ul, captured.RouteContext.DeadlineUnixMs);
            Assert.Equal<ulong>(83, captured.RelocationReplyRouteId);
        }
        finally
        {
            runtime.GetOrCreateActorState(actorRef.ActorId).Handoff.AbortCapture();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task RemoteActorFrameReceiver_RejectsAuthenticatedPeerAndOriginalSourceMismatch()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            var relayNode = RoutingId.From("relay-source-node");
            node.AdmittedMeshPeers.Add(new MeshNodePeer(
                1,
                MeshPeerSource.Discovery,
                MeshPeerState.Admitted,
                relayNode,
                61,
                1,
                "inproc://relay-source-node",
                1,
                0,
                1));
            node.SetLocalActorAuthorityFence(actorRef, 41, 51);
            var source = new ZLinkServiceWireCodec.RequestSourceFence(
                "caller-owner",
                71,
                RoutingId.From("different-caller"),
                72);

            var peerMismatch = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                runtime.DispatchRemoteActorFrameAsync(
                    actorRef.ActorId, actorRef.Generation, actorRef.NodeRid, 1, 41, 51,
                    RoutingId.From("unauthenticated-relay"), relayNode, 61,
                    RoutingId.From("caller-node"), 72, default, source,
                    Array.Empty<byte>(),
                    new MeshOperationId(81, 82), 1, 83, 1, "reply-capability",
                    0,
                    ZLinkStreamProtocolDefaults.EncodeHeader(CreateHeader("request")).ToArray(),
                    Encoding.UTF8.GetBytes("payload"),
                    CancellationToken.None).AsTask());
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, peerMismatch.Kind);

            var sourceMismatch = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                runtime.DispatchRemoteActorFrameAsync(
                    actorRef.ActorId, actorRef.Generation, actorRef.NodeRid, 1, 41, 51,
                    relayNode, relayNode, 61,
                    RoutingId.From("caller-node"), 72, default, source,
                    Array.Empty<byte>(),
                    new MeshOperationId(81, 82), 1, 83, 1, "reply-capability",
                    0,
                    ZLinkStreamProtocolDefaults.EncodeHeader(CreateHeader("request")).ToArray(),
                    Encoding.UTF8.GetBytes("payload"),
                    CancellationToken.None).AsTask());
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, sourceMismatch.Kind);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task RemoteActorFrame_BoundSessionWithoutRouteLease_UsesBindingFenceForStaleHandling()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            var relayNode = RoutingId.From("session-node");
            var sessionRid = RoutingId.From("session-rid");
            node.AdmittedMeshPeers.Add(new MeshNodePeer(
                1,
                MeshPeerSource.Discovery,
                MeshPeerState.Admitted,
                relayNode,
                1,
                1,
                "inproc://session-node",
                1,
                0,
                1));
            RegisterProbeActor(runtime, actorRef);
            runtime.BindActorSession(
                actorRef.ActorId,
                relayNode,
                sessionRid,
                "live-binding",
                bindingGeneration: 2,
                objectGeneration: actorRef.Generation,
                authorityOwnerGeneration: 3,
                meshName: "entry",
                targetNodeGeneration: 1,
                ownerLeaseGeneration: 4,
                sessionOwnerNodeGeneration: 1,
                acceptedHighWater: 1);

            var staleMetadata = ZLinkActorBoundSessionHandoffMetadata.Encode(
                new ZLinkActorBoundSessionHandoffFence(
                    actorRef.ActorId,
                    actorRef.Generation,
                    sessionRid,
                    "replaced-binding",
                    BindingGeneration: 2,
                    SessionSequence: 1));
            var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
                "session-owner",
                1,
                relayNode,
                NodeGeneration: 1);
            var requestHeader = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Request,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                new ZlinkStreamRequestSeq(7),
                "stale-bound-request",
                ZlinkStreamMetadata.Empty);
            var encodedRequestHeader = ZLinkStreamProtocolDefaults.EncodeHeader(requestHeader).ToArray();
            var decodedRequestHeader = ZLinkStreamProtocolDefaults.DecodeHeader(encodedRequestHeader);
            Assert.Equal(ZlinkStreamMessageKind.Request, decodedRequestHeader.Kind);
            Assert.NotNull(decodedRequestHeader.RequestSeq);

            var directReplies = new List<byte[]>();
            await ZLinkActorBoundSessionRelay.ReplyStaleActorAsync(
                runtime,
                actorRef,
                relayNode,
                sessionRid,
                requestId: 7,
                flags: ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                replyCapability: "stale-reply",
                decodedRequestHeader,
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NotFound,
                    "session relay authority identity is stale."),
                CancellationToken.None,
                directReply: (parts, _) =>
                {
                    directReplies.Add(
                        parts.Single().AsReadOnlySpan().ToArray());
                    return SubmitResult.Ok;
                });

            var directReply = Assert.Single(directReplies);
            var directReplyPayload = DecodeReplyFrame<ZLinkStreamWireError>(directReply);
            Assert.Equal(
                "not_found",
                directReplyPayload.Payload.Code);

            await runtime.DispatchRemoteActorFrameAsync(
                actorRef.ActorId,
                actorRef.Generation,
                actorRef.NodeRid,
                targetNodeGeneration: 1,
                authorityOwnerGeneration: 3,
                ownerLeaseGeneration: 0,
                authenticatedRelayNodeRid: relayNode,
                relayNodeRid: relayNode,
                relayNodeGeneration: 1,
                sourceNodeRid: relayNode,
                sourceNodeGeneration: 1,
                sourceSessionRid: sessionRid,
                requestSource,
                staleMetadata,
                new MeshOperationId(81, 82),
                messageFollowHopCount: 0,
                replyRequestId: 7,
                replyFlags: ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                replyCapability: "stale-reply",
                deadlineUnixMs: ulong.MaxValue,
                encodedRequestHeader,
                Encoding.UTF8.GetBytes("request-body"),
                CancellationToken.None);

            Assert.Empty(node.NoBindReplies);
            Assert.Empty(node.BoundSessionReplies);

            var oneWayHeader = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                null,
                "stale-bound-send",
                ZlinkStreamMetadata.Empty);
            await runtime.DispatchRemoteActorFrameAsync(
                actorRef.ActorId,
                actorRef.Generation,
                actorRef.NodeRid,
                targetNodeGeneration: 1,
                authorityOwnerGeneration: 3,
                ownerLeaseGeneration: 0,
                authenticatedRelayNodeRid: relayNode,
                relayNodeRid: relayNode,
                relayNodeGeneration: 1,
                sourceNodeRid: relayNode,
                sourceNodeGeneration: 1,
                sourceSessionRid: sessionRid,
                requestSource,
                staleMetadata,
                new MeshOperationId(81, 83),
                messageFollowHopCount: 0,
                replyRequestId: 0,
                replyFlags: 0,
                replyCapability: null,
                deadlineUnixMs: ulong.MaxValue,
                ZLinkStreamProtocolDefaults.EncodeHeader(oneWayHeader).ToArray(),
                Encoding.UTF8.GetBytes("send-body"),
                CancellationToken.None);

            Assert.Empty(node.NoBindReplies);
            Assert.Empty(node.ActorSends);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public void BoundSessionRelay_RequiresAnExactRequestSourceFence()
    {
        var missing = Assert.Throws<ZLinkFrameworkException>(() =>
            ZLinkFrameworkRuntime.ValidateRemoteActorFrameSource(
                "actor-session-send",
                routeContext: default,
                RoutingId.From("session-node"),
                sourceNodeGeneration: 0,
                requestSource: null));
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, missing.Kind);

        ZLinkFrameworkRuntime.ValidateRemoteActorFrameSource(
            "actor-session-send",
            routeContext: default,
            RoutingId.From("session-node"),
            sourceNodeGeneration: 7,
            new ZLinkServiceWireCodec.RequestSourceFence(
                "session-owner",
                8,
                RoutingId.From("session-node"),
                7));
    }

    [Fact]
    public async Task SessionActorFullFrameUsesOneBindingOwnedAsyncAdmission()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false);
        try
        {
            node.NodeSendAttempts.Clear();
            node.NodeSendResults.Clear();
            node.NodeSendResults.Enqueue(SubmitResult.Backpressured);
            node.NodeSendResults.Enqueue(SubmitResult.Ok);
            var sessionRid = RoutingId.From("session-relay-retry");
            var context = new ZLinkSessionContext(
                runtime,
                new ZLinkManagedStream(
                    new RelayStreamSocket(),
                    sessionRid,
                    runtime.Registration.Codecs,
                    "test"),
                new RelaySessionHandlerRegistry(),
                static () => ValueTask.CompletedTask,
                static _ => ValueTask.CompletedTask);
            const string actorId = "actor-relay-retry";
            const string bindingToken = "binding-relay-retry";
            var actor = new ActorRef(
                actorId,
                1,
                "entry",
                RoutingId.From("remote-actor-node"));
            var bound = new ZLinkSessionActor(
                context,
                actorId,
                sessionRid,
                bindingToken);
            _ = runtime.BindSessionActor(
                actorId,
                context,
                bindingToken,
                bound,
                bindingGeneration: 1,
                route: ZLinkSessionBindingRoute.Create(
                    actor,
                    "entry",
                    targetNodeGeneration: 2,
                    authorityOwnerGeneration: 3,
                    ownerLeaseGeneration: 4),
                sessionOwnerNodeGeneration: 5);
            var header = CreateHeader("retry-frame");
            var expectedHeader = ZLinkStreamProtocolDefaults.EncodeHeader(header).ToArray();
            var expectedBody = Encoding.UTF8.GetBytes("body");
            using var body = Message.From(expectedBody);

            await context.ActorCoordinator.RelayToActorAsync(
                bound,
                header,
                body,
                static (_, _, _) => ValueTask.CompletedTask,
                CancellationToken.None);

            Assert.Equal(1, node.NodeSendAsyncCalls);
            Assert.Equal(2, node.NodeSendAttempts.Count);
            foreach (var attempt in node.NodeSendAttempts)
            {
                var relay = ZLinkFrameworkJsonPayloadCodec
                    .Deserialize<ZLinkRemoteActorFrameRelay>(attempt[1]);
                Assert.NotNull(relay);
                Assert.Equal(expectedHeader, relay.Header);
                Assert.Equal(expectedBody, relay.Body);
                Assert.Equal("entry-node", RoutingId.FromHex(relay.RelayNodeRid).ToString());
                Assert.Equal<ulong>(1, relay.RelayNodeGeneration);
                Assert.Equal<ulong>(1, relay.SourceNodeGeneration);
                Assert.False(string.IsNullOrWhiteSpace(relay.RequestSourceOwnerId));
                Assert.True(relay.RequestSourceLeaseGeneration > 0);
                Assert.Equal("entry-node", RoutingId.FromHex(
                    Assert.IsType<string>(relay.RequestSourceNodeRid)).ToString());
                Assert.Equal<ulong>(1, relay.RequestSourceNodeGeneration);
                Assert.True(ZLinkActorBoundSessionHandoffMetadata.TryDecode(
                    relay.ApplicationMetadata,
                    out var binding));
                Assert.Equal(actorId, binding.ActorId);
                Assert.Equal(sessionRid, binding.SessionRid);
                Assert.Equal(bindingToken, binding.BindingToken);
                Assert.Equal<ulong>(1, binding.BindingGeneration);
                Assert.Equal<ulong>(1, binding.SessionSequence);
            }
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task SessionRelocationHoldPreservesSendAndRequestFramesAcrossConsecutiveRouteSwitches()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false);
        try
        {
            var sessionRid = RoutingId.From("session-relocation-hold");
            var context = new ZLinkSessionContext(
                runtime,
                new ZLinkManagedStream(
                    new RelayStreamSocket(),
                    sessionRid,
                    runtime.Registration.Codecs,
                    "test"),
                new RelaySessionHandlerRegistry(),
                static () => ValueTask.CompletedTask,
                static _ => ValueTask.CompletedTask);
            const string actorId = "actor-relocation-hold";
            const string bindingToken = "binding-relocation-hold";
            var sourceNode = RoutingId.From("source-actor-node");
            var bound = new ZLinkSessionActor(
                context,
                actorId,
                sessionRid,
                bindingToken);
            _ = runtime.BindSessionActor(
                actorId,
                context,
                bindingToken,
                bound,
                bindingGeneration: 6,
                route: ZLinkSessionBindingRoute.Create(
                    new ActorRef(actorId, 5, "entry", sourceNode),
                    "entry",
                    targetNodeGeneration: 2,
                    authorityOwnerGeneration: 11,
                    ownerLeaseGeneration: 17),
                sessionOwnerNodeGeneration: 1,
                sessionOwnerNodeRid: node.RoutingId,
                sessionOwnerId: "session-owner",
                sessionOwnerLeaseGeneration: 8);

            Assert.True(runtime.TryGetSessionActorBinding(
                actorId,
                bindingToken,
                out var sourceBinding));
            var firstSeal = SessionSeal(
                sourceBinding,
                new ZLinkServiceWireCodec.RelocationWireId(8, 1),
                sourceNode,
                sourceNodeGeneration: 2,
                coordinatorLease: 17);
            _ = await runtime.SealCanonicalSessionActorRouteAsync(
                firstSeal,
                CancellationToken.None);

            var sendHeader = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                null,
                "held-send",
                ZlinkStreamMetadata.Empty);
            var sendBody = Encoding.UTF8.GetBytes("send-body");
            using var sendPayload = Message.From(sendBody);
            var heldSend = context.ActorCoordinator.RelayToActorAsync(
                    bound,
                    sendHeader,
                    sendPayload,
                    static (_, _, _) => ValueTask.CompletedTask,
                    CancellationToken.None)
                .AsTask();
            await Task.Delay(20);
            Assert.False(heldSend.IsCompleted);
            Assert.Empty(node.NodeSendAttempts);

            var targetOne = RoutingId.From("target-actor-one");
            var firstCommit = SessionCommit(
                firstSeal,
                targetOne,
                targetNodeGeneration: 3,
                targetAuthority: 12);
            Assert.True(
                runtime.RouteCanonicalSessionActor(
                    firstCommit,
                    new ZLinkSessionRelocationAuthenticatedRoute(
                        targetOne,
                        3,
                        "entry",
                        12,
                        101)));
            await heldSend;

            var sentRelay = ZLinkFrameworkJsonPayloadCodec
                .Deserialize<ZLinkRemoteActorFrameRelay>(
                    Assert.Single(node.NodeSendAttempts)[1]);
            Assert.NotNull(sentRelay);
            Assert.Equal(sendBody, sentRelay.Body);
            Assert.Equal(
                ZLinkStreamProtocolDefaults.EncodeHeader(sendHeader).ToArray(),
                sentRelay.Header);
            Assert.Equal(0UL, sentRelay.ReplyRequestId);
            Assert.Null(sentRelay.ReplyCapability);
            node.NodeSendAttempts.Clear();

            Assert.True(runtime.TryGetSessionActorBinding(
                actorId,
                bindingToken,
                out var targetOneBinding));
            Assert.Equal(101UL, targetOneBinding.OwnerLeaseGeneration);
            var secondSeal = SessionSeal(
                targetOneBinding,
                new ZLinkServiceWireCodec.RelocationWireId(8, 2),
                targetOne,
                sourceNodeGeneration: 3,
                coordinatorLease: 101);
            Assert.Equal(101UL, secondSeal.Actor.OwnerLeaseGeneration);
            _ = await runtime.SealCanonicalSessionActorRouteAsync(
                secondSeal,
                CancellationToken.None);

            var requestHeader = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Request,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                new ZlinkStreamRequestSeq(77),
                "held-request",
                ZlinkStreamMetadata.Empty);
            var requestBody = Encoding.UTF8.GetBytes("request-body");
            using var requestPayload = Message.From(requestBody);
            var rawReplyCalls = 0;
            var heldRequest = context.ActorCoordinator.RelayToActorAsync(
                    bound,
                    requestHeader,
                    requestPayload,
                    (_, _, _) =>
                    {
                        Interlocked.Increment(ref rawReplyCalls);
                        return ValueTask.CompletedTask;
                    },
                    CancellationToken.None)
                .AsTask();
            await Task.Delay(20);
            Assert.False(heldRequest.IsCompleted);
            Assert.Empty(node.NodeSendAttempts);

            var targetTwo = RoutingId.From("target-actor-two");
            var secondCommit = SessionCommit(
                secondSeal,
                targetTwo,
                targetNodeGeneration: 4,
                targetAuthority: 13);
            Assert.True(
                runtime.RouteCanonicalSessionActor(
                    secondCommit,
                    new ZLinkSessionRelocationAuthenticatedRoute(
                        targetTwo,
                        4,
                        "entry",
                        13,
                        202)));
            await heldRequest;

            var requestRelay = ZLinkFrameworkJsonPayloadCodec
                .Deserialize<ZLinkRemoteActorFrameRelay>(
                    Assert.Single(node.NodeSendAttempts)[1]);
            Assert.NotNull(requestRelay);
            Assert.Equal(requestBody, requestRelay.Body);
            Assert.Equal(
                ZLinkStreamProtocolDefaults.EncodeHeader(requestHeader).ToArray(),
                requestRelay.Header);
            Assert.Equal(77UL, requestRelay.ReplyRequestId);
            Assert.Equal(
                ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                requestRelay.ReplyFlags);
            Assert.False(string.IsNullOrWhiteSpace(
                requestRelay.ReplyCapability));
            Assert.Equal(0, rawReplyCalls);
            Assert.True(ZLinkActorBoundSessionHandoffMetadata.TryDecode(
                requestRelay.ApplicationMetadata,
                out var requestFence));
            Assert.Equal(2UL, requestFence.SessionSequence);
            Assert.True(runtime.TryGetSessionActorBinding(
                actorId,
                bindingToken,
                out var targetTwoBinding));
            Assert.Equal(targetTwo, targetTwoBinding.Route.Ref.NodeRid);
            Assert.Equal(202UL, targetTwoBinding.OwnerLeaseGeneration);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }

        static ZLinkServiceWireCodec.SessionRelocationSealRecord SessionSeal(
            ZLinkSessionBindingEntry binding,
            ZLinkServiceWireCodec.RelocationWireId relocationId,
            RoutingId sourceNode,
            ulong sourceNodeGeneration,
            ulong coordinatorLease) =>
            new(
                relocationId,
                new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                    "coordinator",
                    coordinatorLease,
                    sourceNode,
                    sourceNodeGeneration,
                    $"store-{relocationId.High}-{relocationId.Low}"),
                1,
                new ZLinkServiceWireCodec.SessionActorRouteFenceRecord(
                    new ZLinkServiceWireCodec.SessionActorIdentityRecord(
                        binding.ActorRef.ActorId,
                        binding.ObjectGeneration),
                    binding.Route.Ref.NodeRid,
                    binding.TargetNodeGeneration,
                    binding.AuthorityOwnerGeneration,
                    binding.OwnerLeaseGeneration),
                new ZLinkServiceWireCodec.SessionOwnerFenceRecord(
                    binding.SessionOwnerNodeRid,
                    binding.SessionOwnerNodeGeneration,
                    binding.SessionOwnerId,
                    binding.SessionOwnerLeaseGeneration,
                    binding.ActorRef.SessionRid,
                    binding.BindingGeneration));

        static ZLinkServiceWireCodec.SessionRelocationRouteRecord SessionCommit(
            ZLinkServiceWireCodec.SessionRelocationSealRecord seal,
            RoutingId targetNode,
            ulong targetNodeGeneration,
            ulong targetAuthority) =>
            new(
                seal.RelocationId,
                seal.Coordinator,
                2,
                seal.Actor.Actor,
                seal.Session,
                ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Commit(
                    seal.Actor.AuthorityOwnerGeneration,
                    targetAuthority,
                    targetNode,
                    targetNodeGeneration));
    }

    // Regression for 4c8036a494 (B1-dotnet 과잉 검증 제거), which deleted the
    // ZLinkSessionOutboundAdmissionKind.Retained branch on both
    // ZLinkActorBoundSessionCoordinator call sites. Per
    // 04-session/02-session-actor-binding.ko.md §8.1, a Retained admission is
    // an *acceptance* (the aggregate holds the frame until route commit) and
    // must never be reported as a failed/not-found submit. This test pins
    // an actor outbound push whose tenure is stale against the sealed table
    // route: AdmitOutboundAsync must retain it, the async entry point must
    // report Submitted (not TargetNotFound), hard-cap overload must preserve
    // Backpressured, and held frames must reach the session's stream once the
    // relocation route commits.
    [Fact]
    public async Task ActorBoundSessionOutboundSendDuringRelocationSealPreservesAdmissionOutcomesAsync()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false);
        try
        {
            var sessionRid = RoutingId.From("session-outbound-retained-async");
            var sourceNode = RoutingId.From("source-actor-node-outbound-async");
            var stream = new RetainedOutboundCapturingStream(sessionRid);
            var context = new ZLinkSessionContext(
                runtime,
                stream,
                new RelaySessionHandlerRegistry(),
                static () => ValueTask.CompletedTask,
                static _ => ValueTask.CompletedTask);
            const string actorId = "actor-outbound-retained-async";
            const string bindingToken = "binding-outbound-retained-async";
            var bound = new ZLinkSessionActor(
                context,
                actorId,
                sessionRid,
                bindingToken);
            _ = runtime.BindSessionActor(
                actorId,
                context,
                bindingToken,
                bound,
                bindingGeneration: 6,
                route: ZLinkSessionBindingRoute.Create(
                    new ActorRef(actorId, 5, "entry", sourceNode),
                    "entry",
                    targetNodeGeneration: 2,
                    authorityOwnerGeneration: 11,
                    ownerLeaseGeneration: 17),
                sessionOwnerNodeGeneration: 1,
                sessionOwnerNodeRid: node.RoutingId,
                sessionOwnerId: "session-owner",
                sessionOwnerLeaseGeneration: 8);

            // The actor-side outbound snapshot already carries the
            // AuthorityOwnerGeneration the relocation target will commit to
            // (99). Until the seal commits, the table's Route still says 11,
            // so AdmitOutboundAsync must not match Immediate — it must
            // Retain.
            _ = runtime.BindActorSession(
                actorId,
                sessionNodeRid: node.RoutingId,
                sessionRid: sessionRid,
                bindingToken: bindingToken,
                bindingGeneration: 6,
                objectGeneration: 5,
                authorityOwnerGeneration: 99,
                meshName: "entry",
                targetNodeGeneration: 2,
                ownerLeaseGeneration: 17,
                sessionOwnerNodeGeneration: 1,
                sessionOwnerId: "session-owner",
                sessionOwnerLeaseGeneration: 8);

            Assert.True(runtime.TryGetSessionActorBinding(
                actorId,
                bindingToken,
                out var sourceBinding));
            var seal = new ZLinkServiceWireCodec.SessionRelocationSealRecord(
                new ZLinkServiceWireCodec.RelocationWireId(9, 1),
                new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                    "coordinator",
                    17,
                    sourceNode,
                    2,
                    "store-9-1"),
                1,
                new ZLinkServiceWireCodec.SessionActorRouteFenceRecord(
                    new ZLinkServiceWireCodec.SessionActorIdentityRecord(
                        sourceBinding.ActorRef.ActorId,
                        sourceBinding.ObjectGeneration),
                    sourceBinding.Route.Ref.NodeRid,
                    sourceBinding.TargetNodeGeneration,
                    sourceBinding.AuthorityOwnerGeneration,
                    sourceBinding.OwnerLeaseGeneration),
                new ZLinkServiceWireCodec.SessionOwnerFenceRecord(
                    sourceBinding.SessionOwnerNodeRid,
                    sourceBinding.SessionOwnerNodeGeneration,
                    sourceBinding.SessionOwnerId,
                    sourceBinding.SessionOwnerLeaseGeneration,
                    sourceBinding.ActorRef.SessionRid,
                    sourceBinding.BindingGeneration));
            _ = await runtime.SealCanonicalSessionActorRouteAsync(
                seal,
                CancellationToken.None);

            var sendBody = Encoding.UTF8.GetBytes("outbound-retained-body-async");
            using var payload = Message.From(sendBody);

            var submitResult = await runtime.SendActorBoundSessionIfCurrentAsync(
                actorId,
                bindingToken,
                new[] { payload },
                CancellationToken.None);

            //  This is the exact regression: before the fix, Retained fell
            //  through to `_ => ZLinkOneWaySubmitStatus.TargetNotFound`,
            //  silently dropping the push instead of holding it.
            Assert.Equal(
                ZLinkOneWaySubmitStatus.Submitted,
                submitResult.Status);
            // Not delivered yet: the frame is held until the seal commits.
            Assert.Empty(stream.Writes);

            // The retained queue is bounded at 4,096 frames. Fill the rest of
            // that production queue, then prove the hard-overload admission
            // remains Backpressured instead of falling through to
            // TargetNotFound.
            const int retainedOutboundCapacity = 4_096;
            for (var retained = 1; retained < retainedOutboundCapacity; retained++)
            {
                using var retainedPayload = Message.From(sendBody);
                var retainedResult = await runtime.SendActorBoundSessionIfCurrentAsync(
                    actorId,
                    bindingToken,
                    new[] { retainedPayload },
                    CancellationToken.None);
                Assert.Equal(
                    ZLinkOneWaySubmitStatus.Submitted,
                    retainedResult.Status);
            }

            using (var overflowPayload = Message.From(sendBody))
            {
                var overflowResult = await runtime.SendActorBoundSessionIfCurrentAsync(
                    actorId,
                    bindingToken,
                    new[] { overflowPayload },
                    CancellationToken.None);
                Assert.Equal(
                    ZLinkOneWaySubmitStatus.Backpressured,
                    overflowResult.Status);
            }
            Assert.Empty(stream.Writes);

            var targetNode = RoutingId.From("target-actor-node-outbound-async");
            var commit = new ZLinkServiceWireCodec.SessionRelocationRouteRecord(
                seal.RelocationId,
                seal.Coordinator,
                2,
                seal.Actor.Actor,
                seal.Session,
                ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Commit(
                    seal.Actor.AuthorityOwnerGeneration,
                    99,
                    targetNode,
                    2));
            Assert.True(
                runtime.RouteCanonicalSessionActor(
                    commit,
                    new ZLinkSessionRelocationAuthenticatedRoute(
                        targetNode,
                        2,
                        "entry",
                        99,
                        101)));

            Assert.Equal(retainedOutboundCapacity, stream.Writes.Count);
            Assert.All(stream.Writes, written => Assert.Equal(sendBody, written));

            // The adjacent Immediate path has the same contract: a local
            // stream write refusal is Backpressured, not TargetNotFound.
            stream.AcceptWrites = false;
            using var refusedPayload = Message.From(sendBody);
            var refusedResult = await runtime.SendActorBoundSessionIfCurrentAsync(
                actorId,
                bindingToken,
                new[] { refusedPayload },
                CancellationToken.None);
            Assert.Equal(
                ZLinkOneWaySubmitStatus.Backpressured,
                refusedResult.Status);
            Assert.Equal(retainedOutboundCapacity, stream.Writes.Count);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class RetainedOutboundCapturingStream(RoutingId routingId)
        : IZLinkStream
    {
        public string SessionId { get; } = routingId.ToHex();

        public RoutingId? RoutingId { get; } = routingId;

        public string? LocalAddr => null;

        public string? RemoteAddr => null;

        public List<byte[]> Writes { get; } = [];

        public bool AcceptWrites { get; set; } = true;

        public bool Write(
            ZLinkMessage payload,
            SendFlags flags = SendFlags.None)
        {
            if (!AcceptWrites) return false;
            Writes.Add(payload.Decode<byte[]>());
            return true;
        }

        public ValueTask CloseAsync() => ValueTask.CompletedTask;
    }

    [Fact]
    public async Task RetriedRemoteReplyAfterAckLossDeliversExactlyOnceToTheSession()
    {
        var node = new CapturingSpotNode();
        var (runtime, actor) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false);
        try
        {
            var sessionRid = RoutingId.From("reply-once-session");
            var socket = new CountingStreamSocket();
            var context = new ZLinkSessionContext(
                runtime,
                new ZLinkManagedStream(
                    socket,
                    sessionRid,
                    runtime.Registration.Codecs,
                    "test"),
                new RelaySessionHandlerRegistry(),
                static () => ValueTask.CompletedTask,
                static _ => ValueTask.CompletedTask);
            const string bindingToken = "reply-once-binding";
            var bound = new ZLinkSessionActor(
                context,
                actor.ActorId,
                sessionRid,
                bindingToken);
            _ = runtime.BindSessionActor(
                actor.ActorId,
                context,
                bindingToken,
                bound,
                bindingGeneration: 1,
                route: ZLinkSessionBindingRoute.Create(
                    new ActorRef(
                        actor.ActorId,
                        actor.Generation,
                        "entry",
                        actor.NodeRid),
                    "entry",
                    targetNodeGeneration: 2,
                    authorityOwnerGeneration: 3,
                    ownerLeaseGeneration: 4),
                sessionOwnerNodeGeneration: 5);
            var capability = runtime.TrackRemoteSessionActorRequest(
                actor.ActorId,
                requestId: 907,
                bindingToken);
            var responder = RoutingId.From("reply-once-responder");
            var frame = new byte[] { 9, 0, 7 };

            await runtime.DeliverRemoteActorReplyAsync(
                actor.ActorId,
                requestId: 907,
                ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                capability,
                sourceNodeRid: responder,
                responderNodeRid: responder,
                frame,
                CancellationToken.None);
            Assert.Equal(1, socket.SendCount);

            // The relay ACK to the target can be lost; the target retries the
            // exact reply. The session-side claim keeps the terminal indexed
            // so the retry cannot push a duplicate frame to the session.
            await runtime.DeliverRemoteActorReplyAsync(
                actor.ActorId,
                requestId: 907,
                ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                capability,
                sourceNodeRid: responder,
                responderNodeRid: responder,
                frame,
                CancellationToken.None);
            Assert.Equal(1, socket.SendCount);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task BoundSessionFrameWithoutFenceGetsTheMovingRetryTerminalAtCapture()
    {
        var node = new CapturingSpotNode();
        var (runtime, actor) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false);
        try
        {
            var state = runtime.GetOrCreateActorState(actor.ActorId);
            state.Handoff.BeginCanonicalMaintenanceImport("capture-fence", []);
            state.Handoff.MarkAuthorityCommitted(
                "capture-fence",
                actor.Generation,
                actor.Generation);
            _ = state.Handoff.PrepareCanonicalMaintenanceReplay("capture-fence");

            var directReplies = new List<byte[]>();
            var frame = new ZLinkSpotActorFrame(
                actor,
                actor,
                RoutingId.From("fence-source-node"),
                RoutingId.From("fence-session"),
                911,
                ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                new ZLinkBackendActorRouteContext(
                    new MeshOperationId(19, 911),
                    MessageFollowHopCount: 0,
                    TargetNodeGeneration: 2,
                    AuthorityOwnerGeneration: 3,
                    OwnerLeaseGeneration: 4,
                    ReplyRequestId: 911,
                    ReplyFlags: ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                    ReplyCapability: "fence-reply",
                    DeadlineUnixMs: ulong.MaxValue,
                    IsBoundSessionRoute: true),
                new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Request,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.HasRequestSeq,
                    new ZlinkStreamRequestSeq(911),
                    "fence-request",
                    ZlinkStreamMetadata.Empty),
                Message.From("fence-body"),
                sourceNodeGeneration: 0,
                requestSource: null,
                (parts, _) =>
                {
                    directReplies.Add(
                        parts.Single().AsReadOnlySpan().ToArray());
                    return SubmitResult.Ok;
                });
            var pipeline = new ZLinkActorInboundPipeline(
                runtime,
                new ZLinkEntrySpotActorInboundEndpoint(runtime));

            // The bound-session frame lacks its request-source fence, so it
            // cannot be made durable while capture is sealed. The request was
            // never accepted: it stays on the pre-Captured deadline contract
            // and receives the observable moving retry terminal instead of an
            // escaping handoff rejection.
            await pipeline.DispatchAsync(
                new ZLinkSpotActorFrameBatch([frame]),
                CancellationToken.None);

            var reply = Assert.Single(directReplies);
            var decoded = DecodeReplyFrame<ZLinkStreamWireError>(reply);
            Assert.Equal(
                "unavailable",
                decoded.Payload.Code);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Remote_Disconnect_Relay_Allocates_An_Operation_Fence()
    {
        var node = new CapturingSpotNode();
        var (runtime, actor) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false);
        try
        {
            var sessionRid = RoutingId.From("disconnect-session");
            var context = new ZLinkSessionContext(
                runtime,
                new ZLinkManagedStream(
                    new RelayStreamSocket(),
                    sessionRid,
                    runtime.Registration.Codecs,
                    "test"),
                new RelaySessionHandlerRegistry(),
                static () => ValueTask.CompletedTask,
                static _ => ValueTask.CompletedTask);
            var actorRef = new ZLinkSessionActor(
                context,
                actor.ActorId,
                sessionRid,
                "disconnect-binding");
            _ = runtime.BindSessionActor(
                actor.ActorId,
                context,
                actorRef.BindingToken,
                actorRef,
                bindingGeneration: 7,
                route: ZLinkSessionBindingRoute.Create(
                    new ActorRef(
                        actor.ActorId,
                        actor.Generation,
                        "entry",
                        actor.NodeRid),
                    "entry",
                    targetNodeGeneration: 11,
                    authorityOwnerGeneration: 13,
                    ownerLeaseGeneration: 17),
                sessionOwnerNodeGeneration: 19);

            Assert.True(runtime.TryGetSessionActorBinding(
                actor.ActorId,
                out var binding));
            await runtime.NotifyActorDisconnectedAsync(
                binding,
                CancellationToken.None);

            var relay = ZLinkFrameworkJsonPayloadCodec
                .Deserialize<ZLinkRemoteActorFrameRelay>(
                    node.NodeSendAttempts.Single()[1]);
            Assert.NotNull(relay);
            Assert.True(
                relay.OperationIdHigh != 0 || relay.OperationIdLow != 0);
            Assert.Equal(actor.ActorId, relay.ActorId);
            Assert.Equal(RoutingId.From("entry-node").ToHex(), relay.RelayNodeRid);
            Assert.Equal(11UL, relay.TargetNodeGeneration);
            Assert.Equal(13UL, relay.AuthorityOwnerGeneration);
            Assert.Equal(17UL, relay.OwnerLeaseGeneration);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task MessageFollower_StopsFinalPartRetryWhenTheDurationExpires()
    {
        var node = new CapturingSpotNode();
        node.ForwardResults.Enqueue(true);
        for (var attempt = 0; attempt < 100; attempt++) node.ForwardResults.Enqueue(false);
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        try
        {
            var follower = new ZLinkActorMessageFollower(runtime);
            using var body = Message.From(Encoding.UTF8.GetBytes("body"));
            var source = new ZLinkBackendActorRef(RoutingId.From("source-node"), "actor-forward", 1);
            // The runtime's own node rid keeps this on the backend forward
            // path (a remote target now takes the actor-frame relay plane).
            var target = new ZLinkBackendActorRef(RoutingId.From("entry-node"), "actor-forward", 2);
            var messageFollowLease = new ZLinkActorMessageFollowLease(TimeProvider.System);
            messageFollowLease.Commit(TimeSpan.FromSeconds(5));
            var messageFollowRoute = new ZLinkActorMessageFollowRoute(
                source,
                target,
                "entry",
                SourceNodeGeneration: 1,
                TargetNodeGeneration: 1,
                SourceAuthorityOwnerGeneration: 3,
                TargetAuthorityOwnerGeneration: 4,
                SourceOwnerLeaseGeneration: 5,
                TargetOwnerLeaseGeneration: 6,
                Lease: messageFollowLease);

            var disposedBody = Message.From(Encoding.UTF8.GetBytes("disposed"));
            disposedBody.Dispose();
            Assert.Throws<ObjectDisposedException>(() => follower.Enqueue(
                messageFollowRoute,
                RoutingId.From("entry-node"),
                RoutingId.From("session-1"),
                6,
                0,
                default,
                CreateHeader("forward-disposed"),
                disposedBody));

            follower.Enqueue(
                messageFollowRoute,
                RoutingId.From("entry-node"),
                RoutingId.From("session-1"),
                7,
                0,
                default,
                CreateHeader("forward"),
                body);

            await node.FinalPartAttempted.Task.WaitAsync(TimeSpan.FromSeconds(5));

            messageFollowLease.Cancel();
            await Task.Delay(25);
            var attemptsAfterExpiry = node.MessageFollowParts.Count;
            await Task.Delay(25);
            Assert.True(node.MessageFollowParts.Count >= 2);
            Assert.True(node.MessageFollowParts[0].HasMore);
            Assert.All(node.MessageFollowParts.Skip(1), static part => Assert.False(part.HasMore));
            Assert.Equal(attemptsAfterExpiry, node.MessageFollowParts.Count);

            using var nextBody = Message.From(Encoding.UTF8.GetBytes("next"));
            var nextLease = new ZLinkActorMessageFollowLease(TimeProvider.System);
            nextLease.Commit(TimeSpan.FromSeconds(5));
            var nextMessageFollowRoute = messageFollowRoute with { Lease = nextLease };
            await Task.Run(() => follower.Enqueue(
                    nextMessageFollowRoute,
                    RoutingId.From("session-node"),
                    RoutingId.From("session-1"),
                    9,
                    0,
                    default,
                    CreateHeader("forward-next"),
                    nextBody))
                .WaitAsync(TimeSpan.FromSeconds(5));
            nextLease.Cancel();
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task MessageFollower_RetainsPayloadsBeyondOrdinaryExecutionLimits()
    {
        var node = new CapturingSpotNode();
        var forwardStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        using var releaseForward = new ManualResetEventSlim(false);
        node.BeforeForwardActorBoundSessionPart = () =>
        {
            forwardStarted.TrySetResult();
            releaseForward.Wait();
        };
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        try
        {
            var follower = new ZLinkActorMessageFollower(runtime);
            using var body = Message.From(new byte[4096]);
            var lease = new ZLinkActorMessageFollowLease(TimeProvider.System);
            lease.Commit(TimeSpan.FromMinutes(1));
            var route = new ZLinkActorMessageFollowRoute(
                new ZLinkBackendActorRef(
                    RoutingId.From("source-node"),
                    "actor-follow-no-cap",
                    1),
                new ZLinkBackendActorRef(
                    RoutingId.From("entry-node"),
                    "actor-follow-no-cap",
                    2),
                "entry",
                SourceNodeGeneration: 1,
                TargetNodeGeneration: 1,
                SourceAuthorityOwnerGeneration: 3,
                TargetAuthorityOwnerGeneration: 4,
                SourceOwnerLeaseGeneration: 5,
                TargetOwnerLeaseGeneration: 6,
                Lease: lease);
            const int messageCount = 4097;
            var completions = new Task<bool>[messageCount];
            completions[0] = follower.EnqueueTracked(
                route,
                RoutingId.From("entry-node"),
                RoutingId.From("source-session"),
                1,
                0,
                default,
                CreateHeader("follow-no-cap"),
                body);
            await forwardStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

            for (var index = 1; index < completions.Length; index++)
            {
                completions[index] = follower.EnqueueTracked(
                    route,
                    RoutingId.From("entry-node"),
                    RoutingId.From("source-session"),
                    checked((ulong)index + 1),
                    0,
                    default,
                    CreateHeader("follow-no-cap"),
                    body);
            }

            releaseForward.Set();
            var submitted = await Task.WhenAll(completions)
                .WaitAsync(TimeSpan.FromSeconds(30));
            Assert.All(submitted, Assert.True);
            Assert.True(
                (long)messageCount * body.Size
                > 16L * 1024 * 1024);
            lease.Cancel();
        }
        finally
        {
            releaseForward.Set();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task LogicalMulticast_Worker_Retains_Runtime_After_Public_Terminal()
    {
        var node = new CapturingSpotNode();
        var disposeStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        node.DisposeHandler = () =>
        {
            disposeStarted.TrySetResult();
            return ValueTask.CompletedTask;
        };
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        using var publishRelease = new ManualResetEventSlim(false);
        var publishStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var callerOperation = runtime.EnterOperation();
        var workerOperation = runtime.RetainOperationForBackgroundWork();

        var terminal = ZLinkLogicalMulticastSubmitter.SubmitAsync(
            runtime.WorkerPool,
            () =>
            {
                publishStarted.TrySetResult();
                publishRelease.Wait();
            },
            CancellationToken.None,
            CancellationToken.None,
            TimeSpan.FromSeconds(1),
            workerOperation.Dispose,
            new ThrowingRuntimeErrorSink()).AsTask();

        await publishStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(
            SubmitResult.Ok,
            await terminal.WaitAsync(TimeSpan.FromSeconds(5)));
        callerOperation.Dispose();

        Task stop;
        using (ExecutionContext.SuppressFlow())
            stop = Task.Run(async () => await runtime.StopAsync(CancellationToken.None));
        await Task.Delay(25);
        Assert.False(disposeStarted.Task.IsCompleted);

        publishRelease.Set();
        await disposeStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await stop.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task RuntimeStart_WaitsUntilThePreviousStopFinishesDisposal()
    {
        var node = new CapturingSpotNode();
        var disposeStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseDispose = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        node.DisposeHandler = async () =>
        {
            disposeStarted.TrySetResult();
            await releaseDispose.Task;
        };
        var (runtime, _) = await CreateStartedRuntimeAsync(node);

        using (runtime.EnterOperation())
            await Assert.ThrowsAsync<InvalidOperationException>(() =>
                runtime.StopAsync(CancellationToken.None).AsTask());
        Assert.True(runtime.IsStarted);

        using var inFlightOperation = runtime.EnterOperation();
        Task stop;
        using (ExecutionContext.SuppressFlow())
            stop = Task.Run(async () => await runtime.StopAsync(CancellationToken.None));
        await Task.Delay(25);
        Assert.False(disposeStarted.Task.IsCompleted);
        inFlightOperation.Dispose();
        await disposeStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(runtime.IsStarted);
        Assert.Null(runtime.Context);
        Assert.Throws<InvalidOperationException>(() =>
            runtime.GetSpotMonitoringSnapshot("entry"));
        var restart = runtime.StartAsync(CancellationToken.None).AsTask();
        await Task.Delay(25);
        Assert.False(restart.IsCompleted);

        releaseDispose.TrySetResult();
        await stop.WaitAsync(TimeSpan.FromSeconds(5));
        await restart.WaitAsync(TimeSpan.FromSeconds(5));
        node.DisposeHandler = null;
        await runtime.StopAsync(CancellationToken.None);
    }

    [Fact]
    public async Task Relocation_operation_zero_waiter_completes_while_admission_remains_open()
    {
        var (runtime, _) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode());
        try
        {
            using var operation = runtime.EnterOperation();
            var zero = runtime.WaitForAcceptedOperationsForDrainAsync();

            Assert.False(zero.IsCompleted);
            operation.Dispose();
            await zero.WaitAsync(TimeSpan.FromSeconds(5));

            using (runtime.EnterOperation())
            {
            }
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Relocation_fence_rejects_stale_operation_baseline_and_reopens_by_owner()
    {
        var (runtime, _) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode());
        try
        {
            var operationBaseline = runtime.SnapshotOperationAdmissions();
            var actorBaseline = runtime.DrainAdmission.SnapshotActorAdmissions();
            var handoffBaseline = new ZLinkActorHandoffDrainSnapshot(0, true);
            using var operation = runtime.EnterOperation();

            var stale = await runtime.TryBeginRelocationAdmissionFenceAsync(
                    operationBaseline,
                    actorBaseline,
                    handoffBaseline,
                    CancellationToken.None)
                .AsTask();

            Assert.Null(stale);
            operation.Dispose();

            var fence = await runtime.TryBeginRelocationAdmissionFenceAsync(
                    runtime.SnapshotOperationAdmissions(),
                    runtime.DrainAdmission.SnapshotActorAdmissions(),
                    handoffBaseline,
                    CancellationToken.None);

            Assert.NotNull(fence);
            Assert.Throws<InvalidOperationException>(() => runtime.EnterOperation());
            var firstFence = fence.Value;
            Assert.True(runtime.TryReopenRetireAdmissionsAfterRollback(firstFence));

            var nextFence = await runtime.TryBeginRelocationAdmissionFenceAsync(
                    runtime.SnapshotOperationAdmissions(),
                    runtime.DrainAdmission.SnapshotActorAdmissions(),
                    handoffBaseline,
                    CancellationToken.None);

            Assert.NotNull(nextFence);
            Assert.False(runtime.TryReopenRetireAdmissionsAfterRollback(firstFence));
            Assert.True(runtime.TryReopenRetireAdmissionsAfterRollback(nextFence.Value));
            using (runtime.EnterOperation())
            {
            }
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Relocation_fence_commits_while_accepted_operation_is_active()
    {
        var (runtime, _) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode());
        try
        {
            var operation = runtime.EnterOperation();
            try
            {
                var fence = await runtime.TryBeginRelocationAdmissionFenceAsync(
                        runtime.SnapshotOperationAdmissions(),
                        runtime.DrainAdmission.SnapshotActorAdmissions(),
                        new ZLinkActorHandoffDrainSnapshot(0, true),
                        CancellationToken.None)
                    .AsTask();

                Assert.NotNull(fence);
                Assert.False(runtime.IsAcceptingApplicationWork);

                operation.Dispose();
                Assert.True(
                    runtime.TryReopenRetireAdmissionsAfterRollback(fence.Value));
            }
            finally
            {
                operation.Dispose();
            }
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData(0, 1, true, 0)]
    [InlineData(1, 0, true, 1)]
    [InlineData(2, 2, false, 0)]
    public async Task Spot_retire_scheduler_reconciles_uncertain_stage_boundary(
        int publishModeValue,
        int expectedKnowledgeValue,
        bool expectedSourceTerminalized,
        int expectedAbortCalls)
    {
        var publishMode = (SchedulerProbePublishMode)publishModeValue;
        var expectedKnowledge =
            (ZLinkRelocationCommitKnowledge)expectedKnowledgeValue;
        IZLinkLocationRepository? authorityStore = null;
        var relocationStore = new SchedulerProbeRelocationStore();
        var relocationRepository =
            new ZLinkProviderRelocationRepository(relocationStore);
        var target = new SchedulerProbeTarget(
            () => authorityStore
                ?? throw new InvalidOperationException(
                    "The scheduler probe authority store is not initialized."),
            relocationRepository,
            publishMode);
        var (runtime, _) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            includeActorFactory: false,
            userSpotType: typeof(EmptyUserSpot),
            defaultRequestTimeout: TimeSpan.FromMilliseconds(250),
            locationStoreWrapper: store =>
            {
                authorityStore = new ZLinkProviderLocationRepository(store);
                return store;
            },
            retireTarget: target,
            relocationStore: relocationStore);
        try
        {
            await runtime.GetOrCreateAsync<EmptyUserSpot>(
                "scheduler-probe");
            runtime.Registration.SpotNodes["entry"].SpotRelocations[
                typeof(EmptyUserSpot).FullName!] =
                new ZLinkObjectRelocationRegistration(
                    typeof(EmptyUserSpot),
                    new ZLinkObjectPlacementOptions(),
                    PolicyKind: 1,
                    AdapterType: null,
                    AdapterInvoker: null);
            Assert.Single(runtime.GetSpotNodeRuntime("entry").Spots);
            Assert.Equal(
                ZLinkUserSpotExecutionMode.SpotWide,
                runtime.GetSpotNodeRuntime("entry").Spots.Single()
                    .ExecutionMode);
            var descriptor = (await authorityStore!.ListMeshNodesAsync(
                    "entry",
                    default,
                    CancellationToken.None))
                .Items
                .Single(item => item.Rid == RoutingId.From("entry-node"));
            var selection = new ZLinkRelocationTargetSelection(
                ZLinkFrameworkRelocationMode.PlannedMaintenance,
                descriptor.ApplicationVersion);
            var result = await runtime.GetSpotNodeRuntime("entry")
                .Catalog
                .TryRelocateForRetireAsync(
                    selection,
                    ZLinkSpotRelocationPhase.Aggregates,
                    CancellationToken.None);

            Assert.False(result.Completed);
            Assert.Equal(expectedKnowledge, result.CommitKnowledge);
            Assert.Equal(expectedSourceTerminalized, result.SourceTerminalized);
            Assert.Equal(
                ZLinkFrameworkRelocationReason.RelocationFailed,
                result.TerminalReason);
            Assert.Equal(
                expectedKnowledge == ZLinkRelocationCommitKnowledge.Committed
                    ? 1UL
                    : 0UL,
                result.CommittedUnitCount);
            Assert.Equal(1, target.StageCalls);
            Assert.Equal(1, target.PublishCalls);
            Assert.Equal(expectedAbortCalls, target.AbortCalls);
        }
        finally
        {
            if (publishMode == SchedulerProbePublishMode.Unknown)
                await runtime.ForceStopAsync(CancellationToken.None);
            else
                await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task RuntimeStop_AllowsAnAdmittedOperationToScheduleGenerationOwnedCleanup()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        using var operation = runtime.EnterOperation();
        Task stop;
        using (ExecutionContext.SuppressFlow())
            stop = Task.Run(async () => await runtime.StopAsync(CancellationToken.None));

        Assert.True(SpinWait.SpinUntil(() => !runtime.IsStarted, TimeSpan.FromSeconds(5)));
        var cleanupRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.True(runtime.TryRunDetached(
            "generation-cleanup",
            _ =>
            {
                cleanupRan.TrySetResult();
                return ValueTask.CompletedTask;
            }));

        await cleanupRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
        operation.Dispose();
        await stop.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task RuntimeForceStop_Fences_Old_Ambient_Operation_Across_Restart()
    {
        var (runtime, _) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode());
        var oldOperation = runtime.EnterOperation();
        try
        {
            Task forceStop;
            using (ExecutionContext.SuppressFlow())
                forceStop = Task.Run(
                    async () => await runtime.ForceStopAsync(
                        CancellationToken.None));
            await forceStop.WaitAsync(TimeSpan.FromSeconds(5));

            Assert.Throws<InvalidOperationException>(
                () => runtime.EnterOperation());

            await runtime.StartAsync(CancellationToken.None);
            Assert.Throws<InvalidOperationException>(
                () => runtime.EnterOperation());
        }
        finally
        {
            oldOperation.Dispose();
        }

        using (runtime.EnterOperation())
        {
        }
        await runtime.StopAsync(CancellationToken.None);
    }

    [Fact]
    public async Task Detached_Generation_Failure_Remains_With_Origin_After_Restart()
    {
        var (runtime, _) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode());
        Exception? originFailure = null;
        Exception? successorFailure = null;
        var origin = runtime.ErrorSink;
        origin.UnhandledCallbackException +=
            exception => originFailure = exception;
        var reporter = origin.CaptureGenerationReporter();

        await runtime.ForceStopAsync(CancellationToken.None);
        await runtime.StartAsync(CancellationToken.None);
        runtime.ErrorSink.UnhandledCallbackException +=
            exception => successorFailure = exception;
        var failure = new InvalidOperationException(
            "origin generation detached cleanup failed");

        reporter(failure);

        Assert.Same(failure, originFailure);
        Assert.Null(successorFailure);
        await runtime.StopAsync(CancellationToken.None);
    }

    [Fact]
    public async Task Drain_Remainder_Request_Count_Excludes_Non_Request_Operations()
    {
        var (runtime, _) = await CreateStartedRuntimeAsync(new CapturingSpotNode());
        try
        {
            using (runtime.EnterOperation())
            using (runtime.EnterOperation(countAsRequest: true))
            {
                var remainder = runtime.GetDrainRemainderCounts();

                Assert.Equal(1, remainder.Requests);
            }
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task EnterOperation_Nested_Lease_Does_Not_Allocate()
    {
        var (runtime, _) = await CreateStartedRuntimeAsync(new CapturingSpotNode());
        try
        {
            var allocated = await Task.Run(() =>
            {
                using var outer = runtime.EnterOperation();
                using (runtime.EnterOperation())
                {
                }

                var allocatedBefore = GC.GetAllocatedBytesForCurrentThread();
                for (var index = 0; index < 100_000; index++)
                {
                    using var nested = runtime.EnterOperation();
                }
                return GC.GetAllocatedBytesForCurrentThread() - allocatedBefore;
            });

            Assert.Equal(0, allocated);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task RuntimeStop_RejectsOwnedStopBeforeWaitingForAnExternalStopGate()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        using var operation = runtime.EnterOperation();
        Task externalStop;
        using (ExecutionContext.SuppressFlow())
            externalStop = Task.Run(async () => await runtime.StopAsync(CancellationToken.None));

        Assert.True(SpinWait.SpinUntil(() => !runtime.IsStarted, TimeSpan.FromSeconds(5)));
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            runtime.StopAsync(CancellationToken.None).AsTask());

        operation.Dispose();
        await externalStop.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task RuntimeRestart_DoesNotReuseActorStateFromTheStoppedGeneration()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        var previous = runtime.GetOrCreateActorState("generation-actor");

        await runtime.StopAsync(CancellationToken.None);
        Assert.True(previous.ContextInvalidated);
        await runtime.StartAsync(CancellationToken.None);

        var current = runtime.GetOrCreateActorState("generation-actor");
        Assert.NotSame(previous, current);
        await runtime.StopAsync(CancellationToken.None);
    }

    [Fact]
    public async Task EntrySpotDispatch_RejectedOwnedPayloadsAreDisposed()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        var spot = new CapturingSpot();
        var runner = new ZLinkRuntimeTaskRunner(new ThrowingRuntimeErrorSink(), CancellationToken.None);
        await runner.StopAsync();
        var pump = new ZLinkEntrySpotDispatchPump(runtime, activation: null, runner);
        pump.Attach(spot);

        var actorParts = CreateActorRequestParts(actorRef, "request", "discard", requestId: 99, flags: 1);
        var actorBody = actorParts[1].Message;
        spot.RaiseDispatch(new ZLinkBackendSpotDispatchInfo(
            ZLinkBackendSpotDispatchEvent.ActorReadable,
            ActorParts: actorParts));

        Assert.True(SpinWait.SpinUntil(
            () =>
            {
                try
                {
                    _ = actorBody.AsReadOnlySpan();
                    return false;
                }
                catch (ObjectDisposedException)
                {
                    return true;
                }
            },
            TimeSpan.FromSeconds(5)));
        await runtime.StopAsync(CancellationToken.None);
    }

    [Fact]
    public async Task EntrySpotActorIngress_UsesTerminalDispatchInsteadOfFrameworkByteRejection()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        var spot = new CapturingSpot();
        var runner = new ZLinkRuntimeTaskRunner(
            new ThrowingRuntimeErrorSink(),
            CancellationToken.None);
        await using var pump = new ZLinkEntrySpotDispatchPump(
            runtime,
            activation: null,
            runner);
        pump.Attach(spot);
        var parts = CreateActorRequestParts(
            actorRef,
            "request",
            "payload",
            requestId: 42,
            flags: 1);
        var body = parts[1].Message;

        spot.RaiseDispatch(new ZLinkBackendSpotDispatchInfo(
            ZLinkBackendSpotDispatchEvent.ActorReadable,
            ActorParts: parts));

        Assert.True(SpinWait.SpinUntil(
            () => IsDisposed(body),
            TimeSpan.FromSeconds(5)));
        Assert.True(SpinWait.SpinUntil(
            () => node.NoBindReplies.Count == 1,
            TimeSpan.FromSeconds(5)));
        var decoded = DecodeReplyFrame<ZLinkStreamWireError>(
            Assert.Single(Assert.Single(node.NoBindReplies).Parts));
        Assert.Equal(
            "not_found",
            decoded.Payload.Code);
        await runner.StopAsync();
        await runtime.StopAsync(CancellationToken.None);
    }

    [Fact]
    public async Task EntrySpotActorLane_PreservesCoreOwnerUntilTerminalAndSiblingProgress()
    {
        var probe = new DispatchProbe();
        var node = new CapturingSpotNode();
        var (runtime, actorA) = await CreateStartedRuntimeAsync(
            node,
            dispatchProbe: probe);
        var spot = new CapturingSpot();
        var actorB = new ZLinkBackendActorRef(
            RoutingId.From("entry-node"),
            "actor-b",
            1);
        RegisterProbeActor(runtime, actorA);
        RegisterProbeActor(runtime, actorB);
        var runner = new ZLinkRuntimeTaskRunner(
            new ThrowingRuntimeErrorSink(),
            CancellationToken.None);
        await using var pump = new ZLinkEntrySpotDispatchPump(
            runtime,
            activation: null,
            runner);
        pump.Attach(spot);

        spot.RaiseDispatch(new ZLinkBackendSpotDispatchInfo(
            ZLinkBackendSpotDispatchEvent.ActorReadable,
            ActorParts: CreateActorRequestParts(
                actorA,
                "first",
                "first",
                requestId: 0,
                flags: 0,
                kind: ZlinkStreamMessageKind.Send)));
        await probe.ActorAFirstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var overflowParts = CreateActorRequestParts(
            actorA,
            "second",
            "second",
            requestId: 0,
            flags: 0,
            kind: ZlinkStreamMessageKind.Send);
        var overflowBody = overflowParts[1].Message;
        spot.RaiseDispatch(new ZLinkBackendSpotDispatchInfo(
            ZLinkBackendSpotDispatchEvent.ActorReadable,
            ActorParts: overflowParts));
        Assert.False(IsDisposed(overflowBody));
        Assert.False(probe.ActorASecondStarted.Task.IsCompleted);

        spot.RaiseDispatch(new ZLinkBackendSpotDispatchInfo(
            ZLinkBackendSpotDispatchEvent.ActorReadable,
            ActorParts: CreateActorRequestParts(
                actorB,
                "first",
                "first",
                requestId: 0,
                flags: 0,
                kind: ZlinkStreamMessageKind.Send)));
        await probe.ActorBStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        probe.ReleaseActorAFirst.TrySetResult();
        await probe.ActorASecondStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(SpinWait.SpinUntil(
            () => IsDisposed(overflowBody),
            TimeSpan.FromSeconds(5)));
        await runner.StopAsync();
        await runtime.StopAsync(CancellationToken.None);
    }

    [Fact]
    public async Task EntrySpotActorDispatch_ConcurrentActors_StartsOutsideEntrySpotSerialLine_AndKeepsSameActorOrdering()
    {
        var probe = new DispatchProbe();
        var services = new ServiceCollection()
            .AddSingleton(probe)
            .AddTransient<ProbeActorSendHandler>()
            .BuildServiceProvider();
        var activation = CreateActivation(services);
        await using var _ = activation.ConfigureAwait(false);
        activation.Configure();

        Assert.True(activation.TryResolveActorPacket(
            typeof(ProbeActor),
            CreateHeader("first"),
            out var descriptor));
        Assert.NotNull(descriptor);

        var actorA = new ProbeActor("actor-a");
        var actorB = new ProbeActor("actor-b");
        var stateA = new ZLinkActorRuntimeState(actorA.ActorId);
        var stateB = new ZLinkActorRuntimeState(actorB.ActorId);

        var firstA = DispatchAsync(activation, descriptor, stateA, actorA, "first");
        await probe.ActorAFirstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var firstB = DispatchAsync(activation, descriptor, stateB, actorB, "first");
        await probe.ActorBStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var secondA = DispatchAsync(activation, descriptor, stateA, actorA, "second");
        await Task.Delay(100);
        Assert.False(probe.ActorASecondStarted.Task.IsCompleted);

        probe.ReleaseActorAFirst.SetResult();
        await probe.ActorASecondStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await Task.WhenAll(firstA, firstB, secondA).WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(
            new[]
            {
                "actor-a:first:start",
                "actor-b:first:start",
                "actor-a:first:end",
                "actor-a:second:start"
            },
            probe.Events.ToArray());
    }

    [Fact]
    public async Task EntrySpotRouteDispatch_UsesRoutedMessagesAlreadyDrainedByBackendCallback()
    {
        var probe = new RouteDispatchProbe();
        var services = new ServiceCollection()
            .AddSingleton(probe)
            .AddTransient<ProbeRouteHandler>()
            .BuildServiceProvider();
        var spot = new CapturingSpot();
        var (activation, runtime) = CreateActivationWithRuntime(services, spot);
        await using var _ = activation.ConfigureAwait(false);
        activation.Configure();

        var pump = new ZLinkEntrySpotDispatchPump(
            runtime,
            activation,
            new ZLinkRuntimeTaskRunner(new ThrowingRuntimeErrorSink(), CancellationToken.None));
        pump.Attach(spot);

        var received = CreateRoutedReceived("routed-ok");
        spot.RaiseDispatch(new ZLinkBackendSpotDispatchInfo(
            ZLinkBackendSpotDispatchEvent.RouteReadable,
            RoutedMessages: [received]));

        Assert.Equal(
            "routed-ok",
            await probe.Message.Task.WaitAsync(TimeSpan.FromSeconds(5)));
    }

    [Fact]
    public async Task EntrySpot_Timer_And_Route_Handler_Share_One_Serial_Line()
    {
        var probe = new EntryTimerSerialProbe();
        var services = new ServiceCollection()
            .AddSingleton(probe)
            .AddTransient<BlockingProbeRouteHandler>()
            .AddTransient<EntryTimerProbeHandler>()
            .BuildServiceProvider();
        var spot = new CapturingSpot();
        var (activation, _) = CreateActivationWithRuntime(
            services,
            spot,
            typeof(TimerProbeEntrySpot));
        await using var cleanup = activation.ConfigureAwait(false);
        activation.Configure();

        var route = activation.DispatchRouteAsync(
            CreateRoutedReceived("hold"),
            CancellationToken.None).AsTask();
        await probe.RouteStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        await using var timer = await activation.AddTimer<EntryTimerProbeHandler>(
            "entry.serial",
            TimeSpan.FromMilliseconds(1));
        var early = await Task.WhenAny(
            probe.TimerStarted.Task,
            Task.Delay(TimeSpan.FromMilliseconds(50)));
        Assert.NotSame(probe.TimerStarted.Task, early);

        probe.ReleaseRoute.TrySetResult();
        await route.WaitAsync(TimeSpan.FromSeconds(5));
        await probe.TimerStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var events = probe.Events.ToArray();
        Assert.Equal(new[] { "route:start", "route:end" }, events[..2]);
        Assert.All(events[2..], entry => Assert.Equal("timer:start", entry));
    }

    [Fact]
    public async Task Application_signaled_defer_blocks_later_framework_operations_and_continues_without_relocation()
    {
        var services = new ServiceCollection().BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration();
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new ThrowingBackendAdapterFactory(),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var activation = new ZLinkUserSpotActivation(
            runtime,
            services.CreateAsyncScope(),
            new CapturingSpot(),
            "application-signaled",
            RoutingId.From("node"),
            "node",
            "channel",
            TimeSpan.FromSeconds(1),
            TimeSpan.FromSeconds(1),
            ZLinkUserSpotExecutionMode.SpotWide,
            ZLinkSpotRelocationCoordinationMode.ApplicationSignaled);
        var spot = new RelocationReadyProbeSpot(activation);
        activation.AttachSpot(spot);
        await using var cleanup = activation.ConfigureAwait(false);
        var cachedOutbound = activation.Outbound;

        using (var handler =
               ZLinkSpotRelocationReadyHandlerScope.Open(activation))
        {
            var deferred = activation.RelocationReady();
            deferred.Defer();

            var error = Assert.Throws<ZLinkFrameworkException>(
                () => _ = activation.Outbound);
            Assert.Equal(
                ZLinkFrameworkErrorKind.InvalidOperation,
                error.Kind);
            var cachedError = Assert.Throws<ZLinkFrameworkException>(
                () => cachedOutbound.SendToSpot(
                    "another-spot",
                    new ProbeRouteMessage("blocked")));
            Assert.Equal(
                ZLinkFrameworkErrorKind.InvalidOperation,
                cachedError.Kind);
            handler.Complete();
        }

        var completion = await spot.NextCompletion.Task
            .WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(
            ZLinkSpotRelocationReadyOutcome.Continued,
            completion.Outcome);
    }

    [Fact]
    public async Task Application_signaled_turn_installs_its_seal_before_the_next_turn_and_completes_before_abort()
    {
        var services = new ServiceCollection().BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration();
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new ThrowingBackendAdapterFactory(),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var activation = new ZLinkUserSpotActivation(
            runtime,
            services.CreateAsyncScope(),
            new CapturingSpot(),
            "application-signaled-cut",
            RoutingId.From("node"),
            "node",
            "channel",
            TimeSpan.FromSeconds(1),
            TimeSpan.FromSeconds(1),
            ZLinkUserSpotExecutionMode.SpotWide,
            ZLinkSpotRelocationCoordinationMode.ApplicationSignaled);
        var spot = new RelocationReadyProbeSpot(activation);
        activation.AttachSpot(spot);
        await using var cleanup = activation.ConfigureAwait(false);

        var pendingSeal = activation.WaitForRelocationReadyTurnAsync(
            CancellationToken.None);
        using (var handler =
               ZLinkSpotRelocationReadyHandlerScope.Open(activation))
        {
            activation.RelocationReady().Defer();
            handler.Complete();
        }

        var seal = await pendingSeal.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(activation.IsRelocationReady);
        await activation.CompleteRelocationReadyBeforeAbortAsync(
            seal,
            CancellationToken.None);
        Assert.False(activation.IsRelocationReady);
        Assert.Equal(
            ZLinkSpotRelocationReadyOutcome.Continued,
            (await spot.NextCompletion.Task.WaitAsync(
                TimeSpan.FromSeconds(5))).Outcome);
        Assert.True(activation.AbortRelocation(seal));
        Assert.True(activation.IsRelocationReady);
    }

    [Fact]
    public async Task Application_signaled_target_callback_retries_before_admission_opens()
    {
        var services = new ServiceCollection().BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration();
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new ThrowingBackendAdapterFactory(),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var activation = new ZLinkUserSpotActivation(
            runtime,
            services.CreateAsyncScope(),
            new CapturingSpot(),
            "application-signaled-target",
            RoutingId.From("node"),
            "node",
            "channel",
            TimeSpan.FromSeconds(1),
            TimeSpan.FromSeconds(1),
            ZLinkUserSpotExecutionMode.SpotWide,
            ZLinkSpotRelocationCoordinationMode.ApplicationSignaled);
        var spot = new RelocationReadyProbeSpot(
            activation,
            failFirstRelocatedCallback: true);
        activation.AttachSpot(spot);
        await using var cleanup = activation.ConfigureAwait(false);
        var seal = await activation.SealRelocationAsync(
            CancellationToken.None);

        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            activation.InvokeTargetRelocationReadyCompletedAsync(
                    seal,
                    CancellationToken.None)
                .AsTask());
        Assert.False(activation.IsRelocationReady);

        await activation.InvokeTargetRelocationReadyCompletedAsync(
            seal,
            CancellationToken.None);
        Assert.False(activation.IsRelocationReady);
        Assert.Equal(2, spot.RelocatedCallbackCount);
        Assert.All(
            spot.AdmissionWasClosed,
            Assert.True);

        Assert.True(activation.AbortRelocation(seal));
    }

    [Fact]
    public async Task User_spot_concurrent_dispose_callers_wait_for_native_cleanup()
    {
        var services = new ServiceCollection().BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration();
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new ThrowingBackendAdapterFactory(),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(services.GetRequiredService<IServiceScopeFactory>(), registration));
        var cleanupStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseCleanup = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var nativeSpot = new CapturingSpot
        {
            DisposeHandler = async () =>
            {
                cleanupStarted.TrySetResult();
                await releaseCleanup.Task.ConfigureAwait(false);
            }
        };
        var activation = new ZLinkUserSpotActivation(
            runtime,
            services.CreateAsyncScope(),
            nativeSpot,
            "spot",
            RoutingId.From("node"),
            "node",
            "channel",
            TimeSpan.FromSeconds(1),
            TimeSpan.FromSeconds(1));
        activation.AttachSpot(new EmptyUserSpot(activation));

        var first = activation.DisposeAsync().AsTask();
        await cleanupStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = activation.DisposeAsync().AsTask();
        Assert.False(first.IsCompleted);
        Assert.False(second.IsCompleted);

        releaseCleanup.TrySetResult();
        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));
        await activation.DisposeAsync();
    }

    [Fact]
    public async Task UserSpotMemberActorIsExcludedFromStandaloneRetireInventory()
    {
        var services = new ServiceCollection().BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration();
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new ThrowingBackendAdapterFactory(),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var activation = new ZLinkUserSpotActivation(
            runtime,
            services.CreateAsyncScope(),
            new CapturingSpot(),
            "aggregate-spot",
            RoutingId.From("node"),
            "node",
            "channel",
            TimeSpan.FromSeconds(1),
            TimeSpan.FromSeconds(1));
        activation.AttachSpot(new EmptyUserSpot(activation));
        var standalone = new ZLinkActorRuntimeState("standalone");
        var member = new ZLinkActorRuntimeState("member");
        member.JoinSpot(activation);

        var inventory = ZLinkActorDrainCoordinator.StandaloneActors(
            [standalone, member]);

        Assert.Equal([standalone], inventory);
        await activation.DisposeAsync();
    }

    [Fact]
    public async Task CommittedSourceLeaveClaimsUserSpotMembershipOnceBeforeActorRetirement()
    {
        var services = new ServiceCollection().BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration();
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new ThrowingBackendAdapterFactory(),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var activation = new ZLinkUserSpotActivation(
            runtime,
            services.CreateAsyncScope(),
            new CapturingSpot(),
            "retired-source-membership",
            RoutingId.From("source-node"),
            "source-node",
            "channel",
            TimeSpan.FromSeconds(1),
            TimeSpan.FromSeconds(1),
            ZLinkUserSpotExecutionMode.PerActor);
        var spot = new SourceLeaveProbeSpot(activation);
        activation.AttachSpot(spot);
        await activation.BindDescriptorsAsync(CancellationToken.None);
        var sourceRef = new ZLinkBackendActorRef(
            RoutingId.From("source-node"),
            "retired-source-actor",
            1);
        var actor = RegisterProbeActor(runtime, sourceRef);
        var actorState = runtime.GetOrCreateActorState(sourceRef.ActorId);
        activation.StageRelocatedPerActorMember(actor, actorState);
        activation.PublishRelocatedPerActorMember(actor, actorState);

        await activation.TryNotifyActorLeftAfterCommittedMembershipAsync(
            actor,
            CancellationToken.None);
        await activation.TryNotifyActorLeftAfterCommittedMembershipAsync(
            actor,
            CancellationToken.None);

        Assert.Equal(0, activation.JoinedActorCount);
        Assert.Equal(1, spot.LeaveCount);
        Assert.Same(activation, actorState.LiveActivation);

        var targetRef = new ZLinkBackendActorRef(
            RoutingId.From("target-node"),
            sourceRef.ActorId,
            sourceRef.Generation);
        actorState.BindNativeActorRef(targetRef);
        actorState.Handoff.BeginCapture();
        _ = actorState.Handoff.CutoverCaptureToMessageFollow(
            0,
            sourceRef,
            targetRef,
            "entry",
            sourceNodeGeneration: 1,
            targetNodeGeneration: 2,
            sourceAuthorityOwnerGeneration: 1,
            targetAuthorityOwnerGeneration: 2,
            sourceOwnerLeaseGeneration: 1,
            targetOwnerLeaseGeneration: 2);
        actorState.Handoff.CommitMessageFollow(TimeSpan.FromSeconds(1));
        actorState.RetireMigratedActorInstance(sourceRef);

        Assert.Null(actorState.Actor);
        Assert.Null(actorState.LiveActivation);
        Assert.Equal(0, activation.JoinedActorCount);
        Assert.Equal(1, spot.LeaveCount);
        await activation.DisposeAsync();
    }

    [Fact]
    public async Task PerActor_source_closing_waits_for_message_follow_and_last_member()
    {
        var services = new ServiceCollection().BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration();
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new ThrowingBackendAdapterFactory(),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var activation = new ZLinkUserSpotActivation(
            runtime,
            services.CreateAsyncScope(),
            new CapturingSpot(),
            "per-actor-closing",
            RoutingId.From("source-node"),
            "source-node",
            "channel",
            TimeSpan.FromSeconds(1),
            TimeSpan.FromSeconds(1),
            ZLinkUserSpotExecutionMode.PerActor);
        var spot = new PerActorClosingProbeSpot(activation);
        activation.AttachSpot(spot);
        var actorRef = new ZLinkBackendActorRef(
            RoutingId.From("source-node"),
            "actor-a",
            1);
        var actor = RegisterProbeActor(runtime, actorRef);
        var actorState = runtime.GetOrCreateActorState(actorRef.ActorId);
        activation.StageRelocatedPerActorMember(actor, actorState);
        activation.PublishRelocatedPerActorMember(actor, actorState);
        await activation.PublishPerActorShellRelocationPlanAsync(
            new ZLinkPerActorShellRelocationPlan(
                RoutingId.From("target-node"),
                2,
                new ZLinkLocationOwnerToken("target-owner", 2),
                3,
                DateTimeOffset.UtcNow + TimeSpan.FromSeconds(5)));
        Assert.True(activation.TrySealPerActorShellRelocation(
            out var shellSeal));
        Assert.True(activation.CommitRelocation(
            shellSeal,
            out var held,
            preserveActorExecution: true));
        Assert.Empty(held);
        var messageFollowDrained = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);

        var closing = activation
            .InvokePerActorRelocationClosingAfterDrainAsync(
                messageFollowDrained.Task,
                CancellationToken.None)
            .AsTask();
        Assert.False(spot.ClosingInvoked.Task.IsCompleted);

        messageFollowDrained.TrySetResult();
        var early = await Task.WhenAny(
            spot.ClosingInvoked.Task,
            Task.Delay(TimeSpan.FromMilliseconds(50)));
        Assert.NotSame(spot.ClosingInvoked.Task, early);

        await activation.NotifyActorLeftAfterCommittedMembershipAsync(
            actor,
            CancellationToken.None);
        await closing.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(
            ZLinkSpotCloseReason.RelocationOut,
            await spot.ClosingInvoked.Task);
        await activation.DisposeAsync();
    }

    [Fact]
    public async Task PerActor_published_shell_fences_deferred_join_commit_racing_last_detach()
    {
        var services = new ServiceCollection().BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration();
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new ThrowingBackendAdapterFactory(),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var activation = new ZLinkUserSpotActivation(
            runtime,
            services.CreateAsyncScope(),
            new CapturingSpot(),
            "per-actor-membership-fence",
            RoutingId.From("source-node"),
            "source-node",
            "channel",
            TimeSpan.FromSeconds(1),
            TimeSpan.FromSeconds(1),
            ZLinkUserSpotExecutionMode.PerActor);
        activation.AttachSpot(new EmptyUserSpot(activation));
        var memberRef = new ZLinkBackendActorRef(
            RoutingId.From("source-node"),
            "existing-member",
            1);
        var member = RegisterProbeActor(runtime, memberRef);
        var memberState = runtime.GetOrCreateActorState(memberRef.ActorId);
        activation.StageRelocatedPerActorMember(member, memberState);
        activation.PublishRelocatedPerActorMember(member, memberState);
        await activation.PublishPerActorShellRelocationPlanAsync(
            new ZLinkPerActorShellRelocationPlan(
                RoutingId.From("target-node"),
                2,
                new ZLinkLocationOwnerToken("target-owner", 2),
                3,
                DateTimeOffset.UtcNow + TimeSpan.FromSeconds(5)));
        var candidateRef = new ZLinkBackendActorRef(
            RoutingId.From("source-node"),
            "deferred-candidate",
            1);
        var candidate = RegisterProbeActor(runtime, candidateRef);

        var commit = Task.Run(async () =>
            await activation.CommitActorJoinFromCallerTurnAsync(
                candidate,
                CancellationToken.None));
        var detach = Task.Run(() =>
            activation.DetachRelocatedPerActorMember(member, memberState));

        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            () => commit);
        await detach;
        await activation.WaitForPerActorMembersDrainedAsync(
            CancellationToken.None);
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
        Assert.Equal(ZLinkRetryAdvice.RetryAfterStateChange, failure.RetryAdvice);
        Assert.False(activation.TryGetJoinedActor(
            candidateRef.ActorId,
            out _));
        Assert.Equal(0, activation.JoinedActorCount);
        await activation.DisposeAsync();
    }

    [Fact]
    public async Task PerActor_detached_cleanup_cancellation_invokes_closing_before_scope_teardown()
    {
        var order = new ClosingTeardownOrderProbe();
        var services = new ServiceCollection()
            .AddSingleton(order)
            .AddScoped<ClosingTeardownDependency>()
            .BuildServiceProvider();
        var scope = services.CreateAsyncScope();
        _ = scope.ServiceProvider.GetRequiredService<
            ClosingTeardownDependency>();
        var registration = new ZLinkFrameworkRegistration();
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new ThrowingBackendAdapterFactory(),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var activation = new ZLinkUserSpotActivation(
            runtime,
            scope,
            new CapturingSpot(),
            "per-actor-cancelled-cleanup",
            RoutingId.From("source-node"),
            "source-node",
            "channel",
            TimeSpan.FromSeconds(1),
            TimeSpan.FromSeconds(1),
            ZLinkUserSpotExecutionMode.PerActor);
        activation.AttachSpot(
            new ClosingTeardownProbeSpot(activation, order));
        var actorRef = new ZLinkBackendActorRef(
            RoutingId.From("source-node"),
            "member",
            1);
        var actor = RegisterProbeActor(runtime, actorRef);
        var actorState = runtime.GetOrCreateActorState(actorRef.ActorId);
        activation.StageRelocatedPerActorMember(actor, actorState);
        activation.PublishRelocatedPerActorMember(actor, actorState);
        await activation.PublishPerActorShellRelocationPlanAsync(
            new ZLinkPerActorShellRelocationPlan(
                RoutingId.From("target-node"),
                2,
                new ZLinkLocationOwnerToken("target-owner", 2),
                3,
                DateTimeOffset.UtcNow + TimeSpan.FromSeconds(5)));
        var messageFollow = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        using var cancellation = new CancellationTokenSource();

        var closing = activation
            .InvokePerActorRelocationClosingAfterDrainAsync(
                messageFollow.Task,
                cancellation.Token)
            .AsTask();
        cancellation.Cancel();
        await closing.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(order.ClosingInvoked);
        Assert.False(order.ScopeDisposed);

        await activation.DisposeAsync();
        Assert.True(order.ScopeDisposed);
        Assert.True(order.ClosingPrecededScopeDisposal);
    }

    [Fact]
    public async Task PerActor_rejected_detached_cleanup_uses_shutdown_cancellation_and_finishes_bounded()
    {
        var order = new ClosingTeardownOrderProbe();
        var services = new ServiceCollection()
            .AddSingleton(order)
            .AddScoped<ClosingTeardownDependency>()
            .BuildServiceProvider();
        var scope = services.CreateAsyncScope();
        _ = scope.ServiceProvider.GetRequiredService<
            ClosingTeardownDependency>();
        var registration = new ZLinkFrameworkRegistration();
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new ThrowingBackendAdapterFactory(),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var activation = new ZLinkUserSpotActivation(
            runtime,
            scope,
            new CapturingSpot(),
            "per-actor-rejected-detached-cleanup",
            RoutingId.From("source-node"),
            "source-node",
            "channel",
            TimeSpan.FromSeconds(1),
            TimeSpan.FromSeconds(30),
            ZLinkUserSpotExecutionMode.PerActor);
        activation.AttachSpot(
            new ClosingTeardownProbeSpot(activation, order));
        var actorRef = new ZLinkBackendActorRef(
            RoutingId.From("source-node"),
            "remaining-member",
            1);
        var actor = RegisterProbeActor(runtime, actorRef);
        var actorState = runtime.GetOrCreateActorState(actorRef.ActorId);
        activation.StageRelocatedPerActorMember(actor, actorState);
        activation.PublishRelocatedPerActorMember(actor, actorState);
        await activation.PublishPerActorShellRelocationPlanAsync(
            new ZLinkPerActorShellRelocationPlan(
                RoutingId.From("target-node"),
                2,
                new ZLinkLocationOwnerToken("target-owner", 2),
                3,
                DateTimeOffset.UtcNow + TimeSpan.FromSeconds(5)));

        Assert.True(runtime.ShutdownToken.IsCancellationRequested);
        Assert.False(runtime.TryRunDetached(
            "prove-rejection",
            _ => ValueTask.CompletedTask));

        await ZLinkSpotNodeCatalog.ScheduleRelocatedSourceCleanupAsync(
                runtime,
                activation)
            .AsTask()
            .WaitAsync(TimeSpan.FromSeconds(2));

        Assert.True(order.ClosingInvoked);
        Assert.True(order.ScopeDisposed);
        Assert.True(order.ClosingPrecededScopeDisposal);
        Assert.True(activation.IsDisposed);
    }

    [Fact]
    public async Task Entry_spot_concurrent_dispose_callers_wait_for_scope_cleanup_failure()
    {
        var cleanup = new BlockingScopeCleanup();
        var services = new ServiceCollection()
            .AddSingleton(cleanup)
            .AddScoped<BlockingScopeDependency>()
            .BuildServiceProvider();
        var (activation, _) = CreateActivationWithRuntime(
            services,
            new CapturingSpot(),
            typeof(ScopeCleanupEntrySpot));

        var first = activation.DisposeAsync().AsTask();
        await cleanup.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = activation.DisposeAsync().AsTask();
        Assert.False(first.IsCompleted);
        Assert.False(second.IsCompleted);

        cleanup.Release.TrySetResult();
        await Assert.ThrowsAsync<InvalidOperationException>(() => first);
        await Assert.ThrowsAsync<InvalidOperationException>(() => second);
        await Assert.ThrowsAsync<InvalidOperationException>(
            () => activation.DisposeAsync().AsTask());
        Assert.Equal(1, cleanup.DisposeCount);
    }

    [Fact]
    public async Task Entry_spot_concurrent_dispose_callers_wait_for_blocked_serial_handler()
    {
        var probe = new BlockingDisposeRouteProbe();
        var services = new ServiceCollection()
            .AddSingleton(probe)
            .AddTransient<BlockingDisposeRouteHandler>()
            .BuildServiceProvider();
        var (activation, _) = CreateActivationWithRuntime(
            services,
            new CapturingSpot(),
            typeof(BlockingDisposeEntrySpot));
        activation.Configure();

        var dispatch = activation.DispatchRouteAsync(
            CreateRoutedReceived("blocked-dispose"),
            CancellationToken.None).AsTask();
        await probe.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var first = activation.DisposeAsync().AsTask();
        var second = activation.DisposeAsync().AsTask();
        Assert.False(first.IsCompleted);
        Assert.False(second.IsCompleted);

        probe.Release.TrySetResult();
        try
        {
            await dispatch.WaitAsync(TimeSpan.FromSeconds(5));
        }
        catch (OperationCanceledException)
        {
            // Disposal may cancel the active handler before its released
            // continuation completes. Both terminal outcomes are valid; the
            // contract under test is that finalization waits for that outcome.
        }
        await Task.WhenAll(first, second).WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task Spot_Node_And_Catalog_Repeated_Dispose_Callers_Share_Finalization()
    {
        var cleanupStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseCleanup = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var node = new CapturingSpotNode
        {
            DisposeHandler = async () =>
            {
                cleanupStarted.TrySetResult();
                await releaseCleanup.Task.ConfigureAwait(false);
            }
        };
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        try
        {
            var target = runtime.GetSpotNodeRuntime("entry");
            var catalog = target.Catalog;

            var firstCatalog = catalog.DisposeAsync().AsTask();
            var secondCatalog = catalog.DisposeAsync().AsTask();
            Assert.Same(firstCatalog, secondCatalog);
            await Task.WhenAll(firstCatalog, secondCatalog).WaitAsync(TimeSpan.FromSeconds(5));

            var firstNode = target.DisposeAsync().AsTask();
            await cleanupStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var secondNode = target.DisposeAsync().AsTask();
            Assert.Same(firstNode, secondNode);
            Assert.False(secondNode.IsCompleted);

            releaseCleanup.TrySetResult();
            await Task.WhenAll(firstNode, secondNode).WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally
        {
            releaseCleanup.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Spot_catalog_dispose_closes_creation_admission_and_waits_for_accepted_create()
    {
        var probe = new BlockingSpotCreateProbe();
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            userSpotType: typeof(BlockingCreateSpot),
            blockingCreateProbe: probe);
        try
        {
            var creation = runtime.CreateAsync<BlockingCreateSpot>().AsTask();
            await probe.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var catalog = runtime.GetSpotNodeRuntime("entry").Catalog;

            var firstDispose = catalog.DisposeAsync().AsTask();
            var secondDispose = catalog.DisposeAsync().AsTask();
            Assert.Same(firstDispose, secondDispose);
            Assert.False(firstDispose.IsCompleted);
            await Assert.ThrowsAsync<ObjectDisposedException>(async () =>
                await runtime.CreateAsync<BlockingCreateSpot>());
            await Assert.ThrowsAsync<ObjectDisposedException>(async () =>
                await runtime.GetOrCreateAsync<BlockingCreateSpot>("post-close"));

            probe.Release.TrySetResult();
            _ = await creation.WaitAsync(TimeSpan.FromSeconds(5));
            await Task.WhenAll(firstDispose, secondDispose).WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Single(node.CreatedSpots);
            Assert.Equal(1, node.CreatedSpots[0].DisposeCount);
        }
        finally
        {
            probe.Release.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task CreateAsync_And_CloseAsync_Keep_UserSpotGauge_Balanced()
    {
        var deltas = new List<long>();
        using var listener = new MeterListener
        {
            InstrumentPublished = (instrument, owner) =>
            {
                if (instrument.Meter.Name == ZLinkMeters.Framework
                    && instrument.Name == "zlink.spot.count")
                    owner.EnableMeasurementEvents(instrument);
            }
        };
        listener.SetMeasurementEventCallback<long>((_, value, tags, _) =>
        {
            foreach (var tag in tags)
                if (tag.Key == "spot_kind" && Equals(tag.Value, "user"))
                    deltas.Add(value);
        });
        listener.Start();

        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            userSpotType: typeof(EmptyUserSpot));
        try
        {
            var created = await runtime.CreateAsync<EmptyUserSpot>();
            var authority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await runtime.Services.GetRequiredService<ZLinkLocationRuntime>()
                    .Store.ReadAuthorityAsync(
                        ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(created.Spot.SpotId)));
            Assert.Equal(authority.Snapshot.ObjectGeneration, created.Spot.ObjectGeneration);

            Assert.Equal(1, deltas.Sum());
            Assert.True(await runtime.CloseAsync(created.Spot));
            Assert.Equal(0, deltas.Sum());
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task PerActor_relocation_destination_requires_exact_published_shell()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            userSpotType: typeof(EmptyUserSpot),
            userSpotExecutionMode: ZLinkUserSpotExecutionMode.PerActor);
        try
        {
            var created = await runtime.CreateAsync<EmptyUserSpot>();
            var spotNode = runtime.GetSpotNodeRuntime("entry");
            var owner = runtime.Services
                .GetRequiredService<ZLinkLocationRuntime>()
                .OwnerToken;
            var authority = new ZLinkActorAuthorityPayload(
                ZLinkActorAuthorityState.Ready,
                "probe",
                "actor-a",
                created.Spot.SpotId,
                created.Spot.ObjectGeneration,
                ZLinkSpotKind.User,
                owner.OwnerId,
                checked((ulong)owner.LeaseGeneration),
                "entry",
                spotNode.Node.RoutingId,
                spotNode.Node.MeshStatus().LifecycleGeneration);

            var activation =
                ZLinkStandaloneActorRelocationRuntime.ResolveTargetMembership(
                    spotNode,
                    authority);
            Assert.NotNull(activation);
            Assert.Equal(
                ZLinkUserSpotExecutionMode.PerActor,
                activation.ExecutionMode);
            var actor = RegisterProbeActor(runtime, actorRef);
            var actorState = runtime.GetOrCreateActorState(actorRef.ActorId);
            activation.StageRelocatedPerActorMember(actor, actorState);
            Assert.Equal(0, activation.JoinedActorCount);
            Assert.Same(activation, actorState.LiveActivation);
            activation.PublishRelocatedPerActorMember(actor, actorState);
            activation.PublishRelocatedPerActorMember(actor, actorState);
            Assert.Equal(1, activation.JoinedActorCount);
            await activation.PublishPerActorShellRelocationPlanAsync(
                new ZLinkPerActorShellRelocationPlan(
                    spotNode.Node.RoutingId,
                    spotNode.Node.MeshStatus().LifecycleGeneration,
                    owner,
                    1,
                    DateTimeOffset.UtcNow + TimeSpan.FromSeconds(5)));
            var membersDrained =
                activation.WaitForPerActorMembersDrainedAsync(
                    CancellationToken.None);
            Assert.False(membersDrained.IsCompleted);
            activation.DetachRelocatedPerActorMember(actor, actorState);
            await membersDrained;
            Assert.Equal(0, activation.JoinedActorCount);
            Assert.Null(actorState.LiveActivation);
            activation.StageRelocatedPerActorMember(actor, actorState);
            activation.AbortStagedRelocatedPerActorMember(
                actor,
                actorState);
            Assert.Equal(0, activation.JoinedActorCount);
            Assert.Null(actorState.LiveActivation);

            foreach (var invalid in new[]
                     {
                         authority with
                         {
                             CurrentSpotGeneration =
                                 checked(authority.CurrentSpotGeneration + 1)
                         },
                         authority with { CurrentSpotKind = ZLinkSpotKind.Instance },
                         authority with { OwnerId = "wrong-owner" }
                     })
            {
                var error = Assert.Throws<ZLinkFrameworkException>(() =>
                    ZLinkStandaloneActorRelocationRuntime
                        .ResolveTargetMembership(spotNode, invalid));
                Assert.Equal(
                    ZLinkFrameworkErrorKind.DataLost,
                    error.Kind);
            }
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task CloseAsync_DoesNotCloseSpotWhenConcurrentJoinCommitsFirst()
    {
        var probe = new BlockingActorJoinProbe();
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            userSpotType: typeof(BlockingActorJoinSpot),
            blockingActorJoinProbe: probe);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var created = await runtime.CreateAsync<BlockingActorJoinSpot>();
            var catalog = runtime.GetSpotNodeRuntime("entry").Catalog;
            var activation = Assert.Single(
                catalog.Spots,
                candidate => candidate.SpotId == created.Spot.SpotId);

            var join = activation.JoinActorAsync(
                    actor,
                    ZLinkMessage.Empty,
                    CancellationToken.None)
                .AsTask();
            await probe.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));

            var close = runtime.CloseAsync(created.Spot).AsTask();
            Assert.False(close.IsCompleted);

            probe.Release.TrySetResult();
            Assert.True((await join.WaitAsync(TimeSpan.FromSeconds(5))).Accepted);
            Assert.False(await close.WaitAsync(TimeSpan.FromSeconds(5)));
            Assert.NotNull(await catalog.GetAsync(created.Spot.SpotId, CancellationToken.None));
        }
        finally
        {
            probe.Release.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task UserSpotJoin_AuthorityCasFailureNeverPublishesTargetMembership()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            userSpotType: typeof(JoinTargetSpot));
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var created = await runtime.CreateAsync<JoinTargetSpot>();
            var catalog = runtime.GetSpotNodeRuntime("entry").Catalog;
            var activation = Assert.Single(
                catalog.Spots,
                candidate => candidate.SpotId == created.Spot.SpotId);
            var store = runtime.Services.GetRequiredService<ZLinkLocationRuntime>().Store;
            var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorRef.ActorId);
            var current = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await store.ReadAuthorityAsync(key));
            Assert.IsType<ZLinkAuthorityCompareExchangeResult.Deleted>(
                await store.CompareExchangeAuthorityAsync(
                    key,
                    current.Snapshot.StoreVersion,
                    new ZLinkAuthorityMutation.Delete()));

            await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await activation.JoinActorAsync(
                    actor,
                    ZLinkMessage.Empty,
                    CancellationToken.None));

            Assert.False(activation.ContainsActor(actor.Context.ActorId));
            Assert.Equal(0, activation.JoinedActorCount);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task UserSpotJoin_RetriesPostCommitCallbackWithoutRollingBackMembership()
    {
        var probe = new CommittedJoinRetryProbe();
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            userSpotType: typeof(RetryingJoinTargetSpot),
            committedJoinRetryProbe: probe);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var created = await runtime.CreateAsync<RetryingJoinTargetSpot>();
            var catalog = runtime.GetSpotNodeRuntime("entry").Catalog;
            var activation = Assert.Single(
                catalog.Spots,
                candidate => candidate.SpotId == created.Spot.SpotId);

            var result = await activation.JoinActorAsync(
                    actor,
                    ZLinkMessage.Empty,
                    CancellationToken.None)
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5));

            Assert.True(result.Accepted);
            Assert.Equal(2, probe.Attempts);
            Assert.True(activation.ContainsActor(actor.Context.ActorId));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Spot_catalog_dispose_waits_for_accepted_get_or_create_and_shares_cleanup_failure()
    {
        var probe = new BlockingSpotCreateProbe();
        var cleanupFailure = new InvalidOperationException("user spot cleanup failed");
        var node = new CapturingSpotNode
        {
            CreatedSpotFactory = () => new CapturingSpot
            {
                DisposeHandler = () => ValueTask.FromException(cleanupFailure)
            }
        };
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            userSpotType: typeof(BlockingCreateSpot),
            blockingCreateProbe: probe);
        try
        {
            var creation = runtime.GetOrCreateAsync<BlockingCreateSpot>("blocked-create").AsTask();
            await probe.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var catalog = runtime.GetSpotNodeRuntime("entry").Catalog;
            var firstDispose = catalog.DisposeAsync().AsTask();
            var secondDispose = catalog.DisposeAsync().AsTask();
            Assert.Same(firstDispose, secondDispose);
            Assert.False(firstDispose.IsCompleted);
            await Assert.ThrowsAsync<ObjectDisposedException>(async () =>
                await runtime.GetOrCreateAsync<BlockingCreateSpot>("blocked-create"));

            probe.Release.TrySetResult();
            _ = await creation.WaitAsync(TimeSpan.FromSeconds(5));
            var firstFailure = await Assert.ThrowsAsync<InvalidOperationException>(() => firstDispose);
            var secondFailure = await Assert.ThrowsAsync<InvalidOperationException>(() => secondDispose);
            Assert.Same(cleanupFailure, firstFailure);
            Assert.Same(cleanupFailure, secondFailure);
            Assert.Same(firstDispose, catalog.DisposeAsync().AsTask());
            Assert.Single(node.CreatedSpots);
            Assert.Equal(1, node.CreatedSpots[0].DisposeCount);
        }
        finally
        {
            probe.Release.TrySetResult();
            await Assert.ThrowsAsync<InvalidOperationException>(async () =>
                await runtime.StopAsync(CancellationToken.None));
        }
    }

    [Fact]
    public async Task GetOrCreateAsync_CallerCancellationOnlyStopsThatCallersWait()
    {
        var probe = new BlockingSpotCreateProbe();
        var node = new CapturingSpotNode();
        const string spotId = "shared-create-cancellation";
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            userSpotType: typeof(BlockingCreateSpot),
            blockingCreateProbe: probe);
        using var ownerCancellation = new CancellationTokenSource();
        try
        {
            var owner = runtime.GetOrCreateAsync<BlockingCreateSpot>(
                    spotId,
                    ZLinkMessage.Empty,
                    ownerCancellation.Token)
                .AsTask();
            await probe.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));

            var waiter = runtime.GetOrCreateAsync<BlockingCreateSpot>(spotId).AsTask();
            ownerCancellation.Cancel();

            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                () => owner.WaitAsync(TimeSpan.FromSeconds(5)));
            Assert.False(waiter.IsCompleted);

            probe.Release.TrySetResult();
            var result = await waiter.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(spotId, result.Spot.SpotId);
            Assert.Equal(ZLinkSpotCreateState.Existing, result.State);
            Assert.Single(node.CreatedSpots);
        }
        finally
        {
            probe.Release.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Current_Spot_Publish_Emits_Sent_With_Spot_Rid_And_Current_Flow()
    {
        var root = Path.Combine(Path.GetTempPath(), $"zlink-current-publish-{Guid.NewGuid():N}");
        var logPath = Path.Combine(root, "flow.log");
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        runtime.Registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);

        try
        {
            var activation = Assert.IsType<ZLinkEntrySpotActivation>(
                runtime.GetSpotNodeRuntime("entry").EntrySpotActivation);
            await activation.Outbound
                .Publish("entry", "events", new ProbeRouteMessage("published"))
                .Async();

            var header = Assert.IsType<ZLinkEnvelopeHeader>(node.EntrySpotBackend.PublishedHeader);
            Assert.Null(header.CorrelationId);
            Assert.True(ZlinkStreamFlowId.IsValid(header.FlowId));
            Assert.Equal(ZLinkFlowOrigin.Application, header.FlowOrigin);
            // 52-message-flow-tracing.ko.md §3·§9: Logical Multicast와 classic fanout
            // publish는 message-flow event를 만들지 않는다. Flow identity는 envelope
            // header로만 전파한다.
            Assert.False(File.Exists(logPath));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
            if (Directory.Exists(root)) Directory.Delete(root, true);
        }
    }

    [Fact]
    public async Task External_Spot_Publish_Emits_Internal_Publisher_Rid_Without_Correlation()
    {
        var root = Path.Combine(Path.GetTempPath(), $"zlink-external-publish-{Guid.NewGuid():N}");
        var logPath = Path.Combine(root, "flow.log");
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeEntryChannelMembership: true);
        runtime.Registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);

        try
        {
            await new ZLinkSpotPublisherClientService(runtime)
                .Publish("entry", "events", new ProbeRouteMessage("published"))
                .Async();

            Assert.True(SpinWait.SpinUntil(
                () => node.CreatedSpots.Count == 1
                      && node.CreatedSpots[0].PublishedHeader is not null,
                TimeSpan.FromSeconds(5)));
            var publisher = Assert.Single(node.CreatedSpots);
            var header = Assert.IsType<ZLinkEnvelopeHeader>(publisher.PublishedHeader);
            Assert.Null(header.CorrelationId);
            Assert.True(ZlinkStreamFlowId.IsValid(header.FlowId));
            Assert.Equal(ZLinkFlowOrigin.Application, header.FlowOrigin);
            // 52-message-flow-tracing.ko.md §3·§9: Logical Multicast publish는
            // message-flow event를 만들지 않는다. 외부 publisher identity는 envelope
            // header와 내부 publisher Spot으로만 관찰한다.
            Assert.False(File.Exists(logPath));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
            if (Directory.Exists(root)) Directory.Delete(root, true);
        }
    }

    [Fact]
    public async Task RouteMesh_Channel_Index_Ignores_PostStartup_Registration_Mutation()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        runtime.Registration.SpotNodes["entry"].ChannelMemberships.Add(
            new ZLinkMeshChannelMembership { ChannelName = "late" });

        try
        {
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                new ZLinkSpotPublisherClientService(runtime)
                    .Publish("late", "events", new ProbeRouteMessage("published"))
                    .Async()
                    .AsTask());
            Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
            Assert.Empty(node.CreatedSpots);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task EntrySpotActorDispatch_NoBindRequest_RepliesViaNoBind_AndDoesNotBindSession()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);

            await DispatchEntryActorPartsAsync(
                runtime,
                CreateActorRequestParts(actorRef, "request", "ok", requestId: 42, flags: 1),
                CancellationToken.None);

            var reply = Assert.Single(node.NoBindReplies);
            Assert.Equal(actorRef, reply.Actor);
            Assert.Equal(RoutingId.From("entry-node"), reply.SourceNodeRid);
            Assert.Equal(RoutingId.From("source-session"), reply.SourceSessionRid);
            Assert.Equal<ulong>(42, reply.RequestId);
            Assert.Equal<uint>(1, reply.Flags);
            Assert.Empty(node.BoundSessionReplies);
            Assert.False(runtime.TryGetActorBoundSession(actor.ActorId, out _));

            var decoded = DecodeReplyFrame<ProbeReply>(Assert.Single(reply.Parts));
            Assert.Equal(ZlinkStreamMessageKind.Response, decoded.Header.Kind);
            Assert.Equal("ok:actor-a", decoded.Payload.Value);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task EntrySpotActorDispatch_Uses_The_State_Accepted_By_The_Target_Queue()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            _ = RegisterProbeActor(runtime, actorRef);
            var acceptedState = runtime.GetOrCreateActorState(actorRef.ActorId);
            var parts = CreateActorRequestParts(
                actorRef,
                "request",
                "accepted-state",
                requestId: 43,
                flags: 1);
            var frames = ZLinkActorHandoffIngress.CaptureMovingFrames(runtime, parts);
            var manager = Assert.IsType<ZLinkActorSessionManager>(
                typeof(ZLinkFrameworkRuntime)
                    .GetField(
                        "_actorSessionManager",
                        BindingFlags.Instance | BindingFlags.NonPublic)!
                    .GetValue(runtime));
            var registry = Assert.IsType<ZLinkActorSessionRegistry>(
                typeof(ZLinkActorSessionManager)
                    .GetField(
                        "_actorSessions",
                        BindingFlags.Instance | BindingFlags.NonPublic)!
                    .GetValue(manager));
            var actorId = ZLinkActorId.FromBoundary(actorRef.ActorId, "actorId");
            registry.RemoveIfCurrent(actorId, acceptedState);
            var successor = registry.GetOrCreate(actorId);
            successor.BindNativeActorRef(actorRef with { Generation = actorRef.Generation + 1 });

            var pipeline = new ZLinkActorInboundPipeline(
                runtime,
                new ZLinkEntrySpotActorInboundEndpoint(runtime));
            await pipeline.DispatchAsync(frames, CancellationToken.None);

            var reply = Assert.Single(node.NoBindReplies);
            var decoded = DecodeReplyFrame<ProbeReply>(Assert.Single(reply.Parts));
            Assert.Equal("accepted-state:actor-a", decoded.Payload.Value);
            Assert.Null(successor.Actor);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task BoundSessionRequestDuringRelocation_GetsActorMovingTerminal_AndNeverEntersJournal()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            RegisterProbeActor(runtime, actorRef);
            var state = runtime.GetOrCreateActorState(actorRef.ActorId);
            state.Handoff.BeginCapture();

            await DispatchEntryActorPartsAsync(
                runtime,
                CreateActorRequestParts(
                    actorRef,
                    "request",
                    "must-not-run",
                    requestId: 142,
                    flags: 1),
                CancellationToken.None);

            Assert.Empty(state.Handoff.SnapshotFrames());
            var reply = Assert.Single(node.NoBindReplies);
            var decoded = DecodeReplyFrame<ZLinkStreamWireError>(
                Assert.Single(reply.Parts));
            Assert.Equal(ZlinkStreamMessageKind.Error, decoded.Header.Kind);
            Assert.Equal(
                "unavailable",
                decoded.Payload.Code);
        }
        finally
        {
            runtime.GetOrCreateActorState(actorRef.ActorId).Handoff.AbortCapture();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task DirectNoBindTerminalDoesNotRetryTransientSubmitFailure()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        var attempts = 0;
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Request,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.HasRequestSeq,
            new ZlinkStreamRequestSeq(17),
            "direct-terminal",
            ZlinkStreamMetadata.Empty);
        try
        {
            var failure = await Assert.ThrowsAsync<ZlinkSubmitException>(async () =>
                await ZLinkActorBoundSessionRelay.SendReplyAsync(
                runtime,
                actorRef.ActorId,
                actorRef,
                RoutingId.From("source-node"),
                RoutingId.From("source-session"),
                requestId: 19,
                flags: ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                replyCapability: "reply-capability",
                isNoBind: true,
                requestHeader: header,
                reply: ZLinkActorReply.FromError(new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "Actor is moving.")),
                cancellationToken: CancellationToken.None,
                directReply: (_, flags) =>
                {
                    Assert.Equal(SendFlags.DontWait, flags);
                    Interlocked.Increment(ref attempts);
                    return SubmitResult.Backpressured;
                }));

            Assert.Equal(ZlinkSubmitException.ErrorCode.Backpressured, failure.Result);
            Assert.Equal(1, Volatile.Read(ref attempts));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task BoundSessionOneWayWithoutExactFence_IsRejectedDuringRelocation()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            RegisterProbeActor(runtime, actorRef);
            var state = runtime.GetOrCreateActorState(actorRef.ActorId);
            state.Handoff.BeginCapture();

            await Assert.ThrowsAsync<ZLinkActorHandoffRejectedException>(() =>
                DispatchEntryActorPartsAsync(
                    runtime,
                    CreateActorRequestParts(
                        actorRef,
                        "request",
                        "must-not-run",
                        requestId: 0,
                        flags: 0,
                        kind: ZlinkStreamMessageKind.Send),
                    CancellationToken.None).AsTask());

            Assert.Empty(state.Handoff.SnapshotFrames());
            Assert.Empty(node.NoBindReplies);
            Assert.Empty(node.BoundSessionReplies);
        }
        finally
        {
            runtime.GetOrCreateActorState(actorRef.ActorId).Handoff.AbortCapture();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task EntrySpotActorDispatch_HandoffReply_RelaysToTheNodeHoldingTheCompletion()
    {
        var node = new CapturingSpotNode
        {
            NoBindReplyAccepted = false
        };
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            _ = RegisterProbeActor(runtime, actorRef);

            await DispatchEntryActorPartsAsync(
                runtime,
                CreateActorRequestParts(
                    actorRef,
                    "request",
                    "handoff",
                    requestId: 46,
                    flags: 1,
                    sourceNodeRid: "caller-node",
                    sourceSessionRid: null),
                CancellationToken.None);

            Assert.Empty(node.NoBindReplies);
            Assert.Equal(RoutingId.From("caller-node"), node.LastNodeSendTarget);
            Assert.Equal(SendFlags.None, node.LastNodeSendFlags);
            Assert.NotEmpty(node.LastNodeSendParts);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task BoundSession_Submit_Rejects_Missing_Binding_On_The_Caller()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        try
        {
            var boundSession = new ZLinkBoundSessionService(runtime).Create("missing-actor");

            var exception = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await boundSession.Send(new ProbeRouteMessage("push")).Async());

            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, exception.Kind);
            Assert.Empty(node.BoundSessionReplies);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Actor_Send_Submit_Times_Out_When_Transport_Remains_Backpressured()
    {
        var node = new CapturingSpotNode();
        var (runtime, actor) = await CreateStartedRuntimeAsync(node);
        try
        {
            var client = new ZLinkActorClient(runtime);
            var publicActor = new ActorRef(
                actor.ActorId,
                actor.Generation,
                "entry",
                actor.NodeRid);

            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await client.SendToActor(
                        publicActor.ActorId,
                        new ProbeRouteMessage("send"))
                    .Async());

            Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
            Assert.Equal(SendFlags.None, node.LastActorSendFlags);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Actor_Send_Uses_A_Router_Only_Spot_Node()
    {
        var node = new CapturingSpotNode { ActorSendAccepted = true };
        var (runtime, actor) = await CreateStartedRuntimeAsync(node, includeActorFactory: false);
        try
        {
            var client = new ZLinkActorClient(runtime);
            var publicActor = new ActorRef(
                actor.ActorId,
                actor.Generation,
                "entry",
                actor.NodeRid);

            await client.SendToActor(publicActor.ActorId, new ProbeRouteMessage("send")).Async();

            Assert.Single(node.ActorSends);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Global_Spot_Send_Resolves_A_Ready_Store_Row_Without_MeshName()
    {
        var node = new CapturingSpotNode { SpotSendResult = SubmitResult.Ok };
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            includeSpotRoute: true);
        try
        {
            await new ZLinkInstanceSpotSendCall<ProbeRouteMessage>(
                    runtime,
                    "spot-ready",
                    new ProbeRouteMessage("send"))
                .Async();

            var sent = Assert.Single(node.SpotSends);
            Assert.Equal("spot-ready", sent.SpotId);
            Assert.Equal(RoutingId.From("spot-node"), sent.NodeRid);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Global_Spot_Request_Resolves_A_Ready_Store_Row_Without_MeshName()
    {
        var node = new CapturingSpotNode
        {
            SpotRequestHandler = parts =>
            {
                var requestHeader = ZLinkEnvelopeCodec.DecodeHeader(parts);
                return ZLinkEnvelopeCodec.EncodeParts(
                    requestHeader with
                    {
                        Kind = ZLinkMessageKind.Response,
                        MessageName = string.Empty
                    },
                    new ProbeReply("reply"),
                    typeof(ProbeReply),
                    codecs: null);
            }
        };
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false,
            includeSpotRoute: true);
        try
        {
            var reply = await new ZLinkInstanceSpotRequestCall<ProbeRouteMessage>(
                    runtime,
                    "spot-ready",
                    new ProbeRouteMessage("request"))
                .Async<ProbeReply>();

            Assert.Equal("reply", reply.Value);
            var requested = Assert.Single(node.SpotRequests);
            Assert.Equal("spot-ready", requested.SpotId);
            Assert.Equal(RoutingId.From("spot-node"), requested.NodeRid);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Instance_Spot_Request_Refreshes_Stale_Route_Before_Retrying()
    {
        var node = new CapturingSpotNode
        {
            SpotRequestHandler = parts =>
            {
                var requestHeader = ZLinkEnvelopeCodec.DecodeHeader(parts);
                return ZLinkEnvelopeCodec.EncodeParts(
                    requestHeader with
                    {
                        Kind = ZLinkMessageKind.Response,
                        MessageName = string.Empty
                    },
                    new ProbeReply("reply"),
                    typeof(ProbeReply),
                    codecs: null);
            }
        };
        node.SpotRequestResults.Enqueue(RequestResult.NotConnected);
        node.SpotRequestResults.Enqueue(RequestResult.Ok);
        var (runtime, _) = await CreateStartedRuntimeAsync(
            node,
            includeInstanceSpotRoute: true);
        try
        {
            var reply = await new ZLinkInstanceSpotRequestCall<ProbeRouteMessage>(
                    runtime,
                    new InstanceSpotIntentAddress(
                        string.Empty,
                        string.Empty,
                        "spot-ready"),
                    new ProbeRouteMessage("request"))
                .Async<ProbeReply>();

            Assert.Equal("reply", reply.Value);
            Assert.Equal(2, node.SpotRequests.Count);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Actor_Send_TargetNotFound_Invalidates_Only_The_Positive_Route()
    {
        var node = new CapturingSpotNode
        {
            ActorSendResult = SubmitResult.NotFound
        };
        var (runtime, actor) = await CreateStartedRuntimeAsync(
            node,
            includeActorFactory: false);
        try
        {
            var client = new ZLinkActorClient(runtime);
            var rows = runtime.Services.GetRequiredService<ZLinkStoreLocationResolvers>();
            var key = new ZLinkActorLocationKey(actor.ActorId);
            Assert.NotNull(await rows.ResolveActorRowAsync(key));
            Assert.True(rows.HasCachedActorRoute(key));

            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                client.SendToActor(
                        actor.ActorId,
                        new ProbeRouteMessage("stale"))
                    .Async()
                    .AsTask());
            Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);

            Assert.False(rows.HasCachedActorRoute(key));
            Assert.Single(node.ActorSends);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Diagnostics_Off_Actor_Send_Creates_Neither_Wire_Flow_Nor_Activity()
    {
        var node = new CapturingSpotNode { ActorSendAccepted = true };
        var observer = new CapturingMessageFlowObserver();
        var (runtime, actor) = await CreateStartedRuntimeAsync(
            node,
            observer,
            ZLinkDiagnosticsLevel.Off);
        try
        {
            Assert.Equal(
                ZLinkDiagnosticsLevel.Off,
                runtime.Registration.DispatchOptions.Diagnostics.EffectiveLevel);
            Assert.Null(ZLinkFlowContext.Current);
            var client = new ZLinkActorClient(runtime);
            var publicActor = new ActorRef(
                actor.ActorId,
                actor.Generation,
                "entry",
                actor.NodeRid);

            await client.SendToActor(publicActor.ActorId, new ProbeRouteMessage("send")).Async();

            Assert.Null(ZLinkFlowContext.Current);
            var sentPacket = Assert.Single(node.ActorSends);
            var header = ZLinkStreamProtocolDefaults.DecodeHeader(sentPacket.Parts[0]);
            Assert.Null(header.FlowId);
            Assert.Null(header.FlowOrigin);
            await Task.Yield();
            Assert.Empty(observer.Events);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Actor_Client_Request_Emits_Sent_And_Exactly_One_Success_Terminal()
    {
        var node = new CapturingSpotNode
        {
            ActorRequestHandler = parts =>
            {
                var requestHeader = ZLinkStreamProtocolDefaults.DecodeHeader(parts[0].AsReadOnlyMemory());
                var responseHeader = requestHeader with
                {
                    Kind = ZlinkStreamMessageKind.Response,
                    Name = string.Empty
                };
                return
                [
                    Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(responseHeader).Span),
                    Message.From(ZLinkEnvelopeCodec.EncodeJsonBytes(new ProbeReply("reply")))
                ];
            }
        };
        var observer = new CapturingMessageFlowObserver();
        var (runtime, actor) = await CreateStartedRuntimeAsync(node, observer);
        try
        {
            Assert.Null(ZLinkFlowContext.Current);
            var client = new ZLinkActorClient(runtime);
            var publicActor = new ActorRef(
                actor.ActorId,
                actor.Generation,
                "entry",
                actor.NodeRid);

            var reply = await client.RequestToActor(
                    publicActor.ActorId,
                    new ProbeRouteMessage("request"))
                .Async<ProbeReply>();

            Assert.Equal("reply", reply.Value);
            Assert.Null(ZLinkFlowContext.Current);
            var sentPacket = Assert.Single(node.ActorRequests);
            var header = ZLinkStreamProtocolDefaults.DecodeHeader(sentPacket.Parts[0]);
            Assert.True(ZlinkStreamFlowId.IsValid(header.FlowId));
            Assert.Equal(ZlinkStreamFlowOrigin.Application, header.FlowOrigin);
            for (var attempt = 0; attempt < 100
                                      && observer.Events.Count(flow => flow.ActorId == actor.ActorId
                                          && flow.Phase is "sent" or "reply_received") < 2;
                 attempt++)
                await Task.Delay(5);
            var events = observer.Events
                .Where(flow => flow.ActorId == actor.ActorId
                               && flow.Phase is "sent" or "reply_received")
                .ToArray();
            Assert.Equal(["sent", "reply_received"], events.Select(flow => flow.Phase));
            Assert.All(events, observed =>
            {
                Assert.Equal("actor", observed.Surface);
                Assert.Equal("request", observed.MessageKind);
                Assert.Equal(header.FlowId, observed.FlowId);
                Assert.Equal("application", observed.FlowOrigin);
                Assert.Equal(header.CorrelationId, observed.CorrelationId);
                Assert.Equal(actor.ActorId, observed.ActorId);
                Assert.Equal("succeeded", observed.Outcome);
            });
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Actor_Request_Subtracts_Location_Read_From_Total_Timeout()
    {
        var node = new CapturingSpotNode
        {
            ActorRequestHandler = parts =>
            {
                var requestHeader =
                    ZLinkStreamProtocolDefaults.DecodeHeader(
                        parts[0].AsReadOnlyMemory());
                return
                [
                    Message.From(
                        ZLinkStreamProtocolDefaults.EncodeHeader(
                            requestHeader with
                            {
                                Kind = ZlinkStreamMessageKind.Response,
                                Name = string.Empty
                            }).Span),
                    Message.From(
                        ZLinkEnvelopeCodec.EncodeJsonBytes(
                            new ProbeReply("reply")))
                ];
            }
        };
        var (runtime, actor) = await CreateStartedRuntimeAsync(
            node,
            locationStoreWrapper: inner =>
                new DelayedLocationProviderStore(
                    inner,
                    TimeSpan.FromMilliseconds(120)));
        try
        {
            var rows = runtime.Services
                .GetRequiredService<ZLinkStoreLocationResolvers>();
            rows.InvalidateActorRoute(
                new ZLinkActorLocationKey(actor.ActorId));
            var reply = await new ZLinkActorClient(runtime)
                .RequestToActor(
                    actor.ActorId,
                    new ProbeRouteMessage("request"))
                .Timeout(TimeSpan.FromMilliseconds(500))
                .Async<ProbeReply>();

            Assert.Equal("reply", reply.Value);
            Assert.InRange(
                node.LastActorRequestTimeout!.Value,
                TimeSpan.FromMilliseconds(200),
                TimeSpan.FromMilliseconds(450));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Retained_BoundSession_First_Application_Send_Creates_One_Wire_Flow_And_Emits_Sent()
    {
        var node = new CapturingSpotNode();
        var observer = new CapturingMessageFlowObserver();
        var (runtime, actor) = await CreateStartedRuntimeAsync(node, observer);
        try
        {
            runtime.GetOrCreateActorState(actor.ActorId).BindNativeActorRef(actor);
            runtime.BindActorSession(
                actor.ActorId,
                // The runtime's own node rid: a differing rid now means a
                // remote session node and takes the relay plane instead.
                RoutingId.From("entry-node"),
                RoutingId.From("session-rid"),
                ZLinkActorBoundSessionBindingToken.Native(RoutingId.From("session-rid")),
                objectGeneration: actor.Generation,
                authorityOwnerGeneration: 1,
                meshName: "entry",
                targetNodeGeneration: 1,
                ownerLeaseGeneration: 1,
                sessionOwnerNodeGeneration: 1);
            var retained = new ZLinkBoundSessionService(runtime).Create(actor.ActorId);
            Assert.Null(ZLinkFlowContext.Current);

            await retained.Send(new ProbeRouteMessage("push")).Async();

            Assert.Null(ZLinkFlowContext.Current);
            var boundPush = Assert.Single(node.BoundSessionReplies);
            var frame = Assert.Single(boundPush.Parts);
            var header = DecodeFrameHeader(frame);
            Assert.True(ZlinkStreamFlowId.IsValid(header.FlowId));
            Assert.Equal(ZlinkStreamFlowOrigin.Application, header.FlowOrigin);
            var sent = await observer.WaitAsync(TimeSpan.FromSeconds(2));
            Assert.Equal("sent", sent.Phase);
            Assert.Equal("stream", sent.Surface);
            Assert.Equal("send", sent.MessageKind);
            Assert.Equal(header.FlowId, sent.FlowId);
            Assert.Equal("application", sent.FlowOrigin);
            Assert.Equal(header.CorrelationId, sent.CorrelationId);
            Assert.Equal(actor.ActorId, sent.ActorId);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Retained_BoundSession_Preserves_The_Ambient_Lifecycle_Flow()
    {
        var node = new CapturingSpotNode();
        var (runtime, actor) = await CreateStartedRuntimeAsync(node);
        try
        {
            runtime.GetOrCreateActorState(actor.ActorId).BindNativeActorRef(actor);
            runtime.BindActorSession(
                actor.ActorId,
                RoutingId.From("entry-node"),
                RoutingId.From("session-rid"),
                ZLinkActorBoundSessionBindingToken.Native(RoutingId.From("session-rid")),
                objectGeneration: actor.Generation,
                authorityOwnerGeneration: 1,
                meshName: "entry",
                targetNodeGeneration: 1,
                ownerLeaseGeneration: 1,
                sessionOwnerNodeGeneration: 1);
            var retained = new ZLinkBoundSessionService(runtime).Create(actor.ActorId);
            using var lifecycle = ZLinkFlowContext.Enter(
                flowId: null,
                origin: null,
                captureEnabled: true,
                ZLinkFlowOrigin.Lifecycle);
            var root = Assert.IsType<ZLinkFlowValue>(ZLinkFlowContext.Current);

            await retained.Send(new ProbeRouteMessage("push")).Async();

            Assert.Equal(root, ZLinkFlowContext.Current);
            var boundPush = Assert.Single(node.BoundSessionReplies);
            var header = DecodeFrameHeader(Assert.Single(boundPush.Parts));
            Assert.Equal(root.FlowId, header.FlowId);
            Assert.Equal(
                (ZlinkStreamFlowOrigin)(byte)root.Origin,
                header.FlowOrigin);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task BoundSession_Uses_BindingOwned_Async_Admission_Without_Framework_Retry()
    {
        var node = new CapturingSpotNode { BoundSessionSendAccepted = false };
        var (runtime, actor) = await CreateStartedRuntimeAsync(node);
        try
        {
            runtime.GetOrCreateActorState(actor.ActorId).BindNativeActorRef(actor);
            runtime.BindActorSession(
                actor.ActorId,
                // The runtime's own node rid: a differing rid now means a
                // remote session node and takes the relay plane instead.
                RoutingId.From("entry-node"),
                RoutingId.From("session-rid"),
                ZLinkActorBoundSessionBindingToken.Native(RoutingId.From("session-rid")),
                objectGeneration: actor.Generation,
                authorityOwnerGeneration: 1,
                meshName: "entry",
                targetNodeGeneration: 1,
                ownerLeaseGeneration: 1,
                sessionOwnerNodeGeneration: 1);
            var retained = new ZLinkBoundSessionService(runtime).Create(actor.ActorId);

            var pending = retained.Send(new ProbeRouteMessage("queued")).Async().AsTask();
            Assert.Empty(node.BoundSessionReplies);

            runtime.BindActorSession(
                actor.ActorId,
                RoutingId.From("entry-node"),
                RoutingId.From("replacement-session-rid"),
                ZLinkActorBoundSessionBindingToken.Native(
                    RoutingId.From("replacement-session-rid")),
                bindingGeneration: 2,
                objectGeneration: actor.Generation,
                authorityOwnerGeneration: 1,
                meshName: "entry",
                targetNodeGeneration: 1,
                ownerLeaseGeneration: 1,
                sessionOwnerNodeGeneration: 1);

            node.BoundSessionSendAccepted = true;
            node.CompleteBoundSessionAdmission();

            await pending;

            Assert.Single(node.BoundSessionReplies);
            Assert.Equal(1, node.BoundSessionSendAttempts);
            Assert.Equal((ulong)1, node.LastBoundSessionBindingGeneration);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task BoundSession_BindingOwnedAdmissionTimeout_CompletesOnce()
    {
        var node = new CapturingSpotNode { BoundSessionSendAccepted = false };
        var (runtime, actor) = await CreateStartedRuntimeAsync(node);
        try
        {
            runtime.Registration.DefaultSocketSendTimeout =
                TimeSpan.FromMilliseconds(20);
            var retained = CreateNativeBoundSession(runtime, actor);

            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await retained.Send(new ProbeRouteMessage("timeout")).Async());

            Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
            Assert.Equal(1, node.BoundSessionSendAttempts);
            Assert.Empty(node.BoundSessionReplies);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task BoundSession_CallerCancellation_PropagatesOnce()
    {
        var node = new CapturingSpotNode { BoundSessionSendAccepted = false };
        var (runtime, actor) = await CreateStartedRuntimeAsync(node);
        using var cancellation = new CancellationTokenSource();
        try
        {
            var retained = CreateNativeBoundSession(runtime, actor);
            var pending = retained.Send(new ProbeRouteMessage("cancel"))
                .Async(cancellation.Token)
                .AsTask();
            Assert.True(SpinWait.SpinUntil(
                () => node.BoundSessionSendAttempts == 1,
                TimeSpan.FromSeconds(2)));

            cancellation.Cancel();

            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                async () => await pending);
            Assert.Equal(1, node.BoundSessionSendAttempts);
            Assert.Empty(node.BoundSessionReplies);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task BoundSession_ForceShutdown_CompletesOnce()
    {
        var node = new CapturingSpotNode { BoundSessionSendAccepted = false };
        var (runtime, actor) = await CreateStartedRuntimeAsync(node);
        var stopped = false;
        try
        {
            var retained = CreateNativeBoundSession(runtime, actor);
            var pending = retained.Send(new ProbeRouteMessage("shutdown"))
                .Async()
                .AsTask();
            Assert.True(SpinWait.SpinUntil(
                () => node.BoundSessionSendAttempts == 1,
                TimeSpan.FromSeconds(2)));

            var stopping = runtime.ForceStopAsync(CancellationToken.None).AsTask();
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(
                async () => await pending);
            await stopping;
            stopped = true;

            Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, error.Kind);
            Assert.Equal(1, node.BoundSessionSendAttempts);
            Assert.Empty(node.BoundSessionReplies);
        }
        finally
        {
            if (!stopped)
                await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Off_Host_Does_Not_Copy_An_Inbound_Flow_To_The_Next_Actor_Outbound()
    {
        // Spec 27 §4: at Off the runtime neither copies an inbound/ambient flow
        // onto the next message nor adds the two flow fields to the outbound
        // envelope. Only correlation_id survives Off.
        const string inboundFlowId = "0196f7c2-4cb4-7cc8-89d4-2d6aee6fca2d";
        var node = new CapturingSpotNode { ActorSendAccepted = true };
        var (runtime, actor) = await CreateStartedRuntimeAsync(
            node,
            messageFlowMode: ZLinkDiagnosticsLevel.Off);
        try
        {
            var client = new ZLinkActorClient(runtime);
            var publicActor = new ActorRef(
                actor.ActorId,
                actor.Generation,
                "entry",
                actor.NodeRid);
            using (ZLinkFlowContext.EnterExisting(
                       inboundFlowId,
                       ZLinkFlowOrigin.Application))
                await client.SendToActor(publicActor.ActorId, new ProbeRouteMessage("relay")).Async();

            Assert.Null(ZLinkFlowContext.Current);
            var sentPacket = Assert.Single(node.ActorSends);
            var header = ZLinkStreamProtocolDefaults.DecodeHeader(sentPacket.Parts[0]);
            Assert.Null(header.FlowId);
            Assert.Null(header.FlowOrigin);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Off_Host_Without_An_Ambient_Flow_Does_Not_Create_Actor_Wire_Flow()
    {
        var node = new CapturingSpotNode { ActorSendAccepted = true };
        var (runtime, actor) = await CreateStartedRuntimeAsync(
            node,
            messageFlowMode: ZLinkDiagnosticsLevel.Off);
        try
        {
            var client = new ZLinkActorClient(runtime);
            var publicActor = new ActorRef(
                actor.ActorId,
                actor.Generation,
                "entry",
                actor.NodeRid);

            Assert.Null(ZLinkFlowContext.Current);
            await client.SendToActor(publicActor.ActorId, new ProbeRouteMessage("off-no-flow")).Async();

            Assert.Null(ZLinkFlowContext.Current);
            var sentPacket = Assert.Single(node.ActorSends);
            var header = ZLinkStreamProtocolDefaults.DecodeHeader(sentPacket.Parts[0]);
            Assert.Null(header.FlowId);
            Assert.Null(header.FlowOrigin);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task EntrySpotActorDispatch_BoundRequest_UsesBoundSession_AndBindsSession()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            BindExactLocalSession(runtime, actorRef);

            await DispatchEntryActorPartsAsync(
                runtime,
                CreateActorRequestParts(actorRef, "request", "bound", requestId: 0, flags: 0),
                CancellationToken.None);

            Assert.Empty(node.NoBindReplies);
            var boundReply = Assert.Single(node.BoundSessionReplies);
            Assert.Equal(actorRef, boundReply.Actor);
            Assert.Equal(1, node.BoundSessionSendAttempts);
            Assert.Equal(1UL, node.LastBoundSessionBindingGeneration);
            Assert.True(runtime.TryGetActorBoundSession(actor.ActorId, out var boundSession));
            Assert.Equal(RoutingId.From("source-session"), boundSession.SessionRid);

            var decoded = DecodeReplyFrame<ProbeReply>(Assert.Single(boundReply.Parts));
            Assert.Equal(ZlinkStreamMessageKind.Response, decoded.Header.Kind);
            Assert.Equal("bound:actor-a", decoded.Payload.Value);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task EntrySpotActorDispatch_BoundMissingHandler_RepliesWithAnErrorFrame()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            RegisterProbeActor(runtime, actorRef);
            BindExactLocalSession(runtime, actorRef);

            await DispatchEntryActorPartsAsync(
                runtime,
                CreateActorRequestParts(actorRef, "missing-handler", "missing", requestId: 0, flags: 0),
                CancellationToken.None);

            var boundReply = Assert.Single(node.BoundSessionReplies);
            var decoded = DecodeReplyFrame<ZLinkStreamWireError>(Assert.Single(boundReply.Parts));
            Assert.Equal(ZlinkStreamMessageKind.Error, decoded.Header.Kind);
            Assert.Equal(
                "not_found",
                decoded.Payload.Code);
            Assert.Contains("No Spot actor request handler", decoded.Payload.Message, StringComparison.Ordinal);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private static void BindExactLocalSession(
        ZLinkFrameworkRuntime runtime,
        ZLinkBackendActorRef actorRef)
    {
        runtime.BindActorSession(
            actorRef.ActorId,
            RoutingId.From("entry-node"),
            RoutingId.From("source-session"),
            ZLinkActorBoundSessionBindingToken.Native(RoutingId.From("source-session")),
            objectGeneration: actorRef.Generation,
            authorityOwnerGeneration: 1,
            meshName: "entry",
            targetNodeGeneration: 1,
            ownerLeaseGeneration: 1,
            sessionOwnerNodeGeneration: 1);
    }

    private static IZLinkBoundSession CreateNativeBoundSession(
        ZLinkFrameworkRuntime runtime,
        ZLinkBackendActorRef actor)
    {
        var sessionRid = RoutingId.From("bound-session-terminal");
        runtime.GetOrCreateActorState(actor.ActorId).BindNativeActorRef(actor);
        runtime.BindActorSession(
            actor.ActorId,
            RoutingId.From("entry-node"),
            sessionRid,
            ZLinkActorBoundSessionBindingToken.Native(sessionRid),
            objectGeneration: actor.Generation,
            authorityOwnerGeneration: 1,
            meshName: "entry",
            targetNodeGeneration: 1,
            ownerLeaseGeneration: 1,
            sessionOwnerNodeGeneration: 1);
        return new ZLinkBoundSessionService(runtime).Create(actor.ActorId);
    }

    [Fact]
    public async Task EntrySpotActorDispatch_NoBindHandlerException_RepliesNoBindError()
    {
        var node = new CapturingSpotNode();
        var observer = new CapturingMessageFlowObserver();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node, observer);
        try
        {
            RegisterProbeActor(runtime, actorRef);

            await DispatchEntryActorPartsAsync(
                runtime,
                CreateActorRequestParts(actorRef, "throw", "boom", requestId: 43, flags: 1),
                CancellationToken.None);

            var reply = Assert.Single(node.NoBindReplies);
            var decoded = DecodeReplyFrame<ZLinkStreamWireError>(Assert.Single(reply.Parts));
            Assert.Equal(ZlinkStreamMessageKind.Error, decoded.Header.Kind);
            Assert.Equal(nameof(InvalidOperationException), decoded.Payload.Code);
            Assert.Contains("boom", decoded.Payload.Message, StringComparison.Ordinal);
            Assert.Empty(node.BoundSessionReplies);
            Assert.False(runtime.TryGetActorBoundSession(actorRef.ActorId, out _));

            var observed = await observer.WaitAsync("failed", TimeSpan.FromSeconds(2));
            Assert.Equal("failed", observed.Outcome);
            Assert.Equal("actor", observed.Surface);
            Assert.Equal("request", observed.MessageKind);
            Assert.Equal("handler_exception", observed.Reason);
            Assert.Equal("reply_error", observed.Action);
            Assert.Equal("throw", observed.PacketName);
            Assert.Equal("actor-a", observed.ActorId);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task EntrySpotActorDispatch_MalformedRequestPayload_ReportsPayloadDecodeFailure()
    {
        var node = new CapturingSpotNode();
        var observer = new CapturingMessageFlowObserver();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node, observer);
        try
        {
            RegisterProbeActor(runtime, actorRef);

            await DispatchEntryActorPartsAsync(
                runtime,
                CreateActorRequestParts(
                    actorRef,
                    "request",
                    "ignored",
                    requestId: 46,
                    flags: 1,
                    malformedPayload: true),
                CancellationToken.None);

            var reply = Assert.Single(node.NoBindReplies);
            var decoded = DecodeReplyFrame<ZLinkStreamWireError>(Assert.Single(reply.Parts));
            Assert.Equal(ZlinkStreamMessageKind.Error, decoded.Header.Kind);
            Assert.Equal(nameof(JsonException), decoded.Payload.Code);
            Assert.Empty(node.BoundSessionReplies);

            var observed = await observer.WaitAsync("failed", TimeSpan.FromSeconds(2));
            Assert.Equal("actor", observed.Surface);
            Assert.Equal("request", observed.MessageKind);
            Assert.Equal("decode_error", observed.Reason);
            Assert.Equal("reply_error", observed.Action);
            Assert.Equal("request", observed.PacketName);
            Assert.Equal("actor-a", observed.ActorId);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task EntrySpotActorDispatch_NoBindMissingActor_RepliesNoBindError()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            await DispatchEntryActorPartsAsync(
                runtime,
                CreateActorRequestParts(actorRef, "request", "missing", requestId: 44, flags: 1),
                CancellationToken.None);

            var reply = Assert.Single(node.NoBindReplies);
            var decoded = DecodeReplyFrame<ZLinkStreamWireError>(Assert.Single(reply.Parts));
            Assert.Equal(ZlinkStreamMessageKind.Error, decoded.Header.Kind);
            Assert.Contains("not available", decoded.Payload.Message, StringComparison.Ordinal);
            Assert.Empty(node.BoundSessionReplies);
            Assert.False(runtime.TryGetActorBoundSession(actorRef.ActorId, out _));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task EntrySpotActorDispatch_MalformedHeader_DisposesFrame_AndContinuesBatch()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        using var badHeader = Message.From([0x01, 0x02, 0x03]);
        using var badBody = Message.From("discarded-body");
        try
        {
            RegisterProbeActor(runtime, actorRef);
            var validParts = CreateActorRequestParts(actorRef, "request", "ok", requestId: 45, flags: 1);
            var parts = new List<ZLinkBackendActorPart>
            {
                new(
                    actorRef,
                    RoutingId.From("source-node"),
                    RoutingId.From("source-session"),
                    44,
                    1,
                    badHeader,
                    true),
                new(
                    actorRef,
                    RoutingId.From("source-node"),
                    RoutingId.From("source-session"),
                    44,
                    1,
                    badBody,
                    false)
            };
            parts.AddRange(validParts);

            await DispatchEntryActorPartsAsync(
                runtime,
                parts,
                CancellationToken.None);

            Assert.Throws<ObjectDisposedException>(() => badHeader.AsReadOnlySpan());
            Assert.Throws<ObjectDisposedException>(() => badBody.AsReadOnlySpan());
            var reply = Assert.Single(node.NoBindReplies);
            var decoded = DecodeReplyFrame<ProbeReply>(Assert.Single(reply.Parts));
            Assert.Equal("ok:actor-a", decoded.Payload.Value);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task EntrySpotJoin_NotConnected_DisposesNativeReplyPartsBeforeRemoteFallback()
    {
        var node = new CapturingSpotNode();
        using var leakedPart = Message.From("not-connected-reply");
        node.EntrySpotJoinReplyParts = [leakedPart];
        node.EntrySpotJoinResult = new ZLinkBackendActorJoinEntrySpotResult(
            RequestResult.NotConnected,
            0,
            new ZLinkBackendActorRef(RoutingId.From("actor-node"), "actor-a", 1),
            RoutingId.From("remote-node"),
            "entry-spot",
            0,
            0);
        node.NodeRequestFailure = new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.NotFound,
            "The remote entry Spot route is unavailable.");
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);

            var exception = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await runtime.JoinActorEntrySpotAsync(
                    RoutingId.From("remote-node"),
                    actor,
                    ZLinkMessage.Empty,
                    CancellationToken.None));

            Assert.Equal(ZLinkFrameworkErrorKind.NotFound, exception.Kind);
            Assert.Throws<ObjectDisposedException>(() => leakedPart.AsReadOnlySpan());
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task LocalEntrySpotJoin_DoesNotUseAConcreteOnActorJoinMethod()
    {
        var node = new CapturingSpotNode();
        ConfigureNotConnectedEntryJoin(node);
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);

            var result = await runtime.JoinActorEntrySpotAsync(
                RoutingId.From("entry-node"),
                actor,
                ZLinkMessage.Empty,
                CancellationToken.None);

            Assert.IsType<ZLinkActorJoinResult.Accepted>(result);
            Assert.Single(node.CreatedActors);
            Assert.Empty(node.DestroyedActors);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task LocalEntrySpotJoin_DestroyFromJoinedCallback_DefersNativeDestroyUntilCallbackReturns()
    {
        var callbackReturned = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var destroyStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseDestroy = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var destroyObservedReturnedCallback = false;
        var node = new CapturingSpotNode
        {
            DestroyHandler = async (_, cancellationToken) =>
            {
                destroyObservedReturnedCallback = callbackReturned.Task.IsCompleted;
                destroyStarted.TrySetResult();
                await releaseDestroy.Task.WaitAsync(cancellationToken);
            }
        };
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            entrySpotType: typeof(LocalJoinProbeEntrySpot));
        runtime.Services.GetRequiredService<LocalEntryJoinProbe>().JoinedHandler =
            async (context, actor, cancellationToken) =>
            {
                await context.DestroyActorAsync(actor, cancellationToken);
                callbackReturned.TrySetResult();
            };
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var join = runtime.JoinActorEntrySpotAsync(
                    RoutingId.From("entry-node"),
                    actor,
                    ZLinkMessage.Empty,
                    CancellationToken.None)
                .AsTask();

            await destroyStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.True(
                destroyObservedReturnedCallback,
                "Native destroy started while the local Entry Spot joined callback still owned the Actor lifecycle.");
            releaseDestroy.TrySetResult();
            await join.WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally
        {
            releaseDestroy.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task LocalEntrySpotJoin_MissingActivation_DoesNotCreateANativeActor()
    {
        var node = new CapturingSpotNode();
        ConfigureNotConnectedEntryJoin(node);
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            includeEntrySpotActivation: false);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);

            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await runtime.JoinActorEntrySpotAsync(
                    RoutingId.From("entry-node"),
                    actor,
                    ZLinkMessage.Empty,
                    CancellationToken.None));

            Assert.Equal(ZLinkFrameworkErrorKind.NotFound, failure.Kind);
            Assert.Empty(node.CreatedActors);
            Assert.Empty(node.DestroyedActors);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task LocalEntrySpotJoin_IgnoresConcreteOnActorJoinFailure()
    {
        var node = new CapturingSpotNode();
        ConfigureNotConnectedEntryJoin(node);
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            entrySpotType: typeof(LocalJoinProbeEntrySpot));
        runtime.Services.GetRequiredService<LocalEntryJoinProbe>().Handler = _ =>
            ValueTask.FromException<ZLinkSpotActorJoinResult>(new InvalidOperationException("admission failed"));
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);

            var result = await runtime.JoinActorEntrySpotAsync(
                RoutingId.From("entry-node"),
                actor,
                ZLinkMessage.Empty,
                CancellationToken.None);

            Assert.IsType<ZLinkActorJoinResult.Accepted>(result);
            Assert.Single(node.CreatedActors);
            Assert.Empty(node.DestroyedActors);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task LocalEntrySpotJoin_IgnoresConcreteOnActorJoinCancellation()
    {
        var node = new CapturingSpotNode();
        ConfigureNotConnectedEntryJoin(node);
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            entrySpotType: typeof(LocalJoinProbeEntrySpot));
        runtime.Services.GetRequiredService<LocalEntryJoinProbe>().Handler = _ =>
            ValueTask.FromCanceled<ZLinkSpotActorJoinResult>(new CancellationToken(canceled: true));
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);

            var result = await runtime.JoinActorEntrySpotAsync(
                RoutingId.From("entry-node"),
                actor,
                ZLinkMessage.Empty,
                CancellationToken.None);

            Assert.IsType<ZLinkActorJoinResult.Accepted>(result);
            Assert.Single(node.CreatedActors);
            Assert.Empty(node.DestroyedActors);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task LocalEntrySpotJoin_DoesNotEnterConcreteCleanupOnRejection()
    {
        var node = new CapturingSpotNode();
        ConfigureNotConnectedEntryJoin(node);
        var attempts = 0;
        node.DestroyHandler = (_, _) => Interlocked.Increment(ref attempts) == 1
            ? ValueTask.FromException(new InvalidOperationException("destroy failed"))
            : ValueTask.CompletedTask;
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);

            var result = await runtime.JoinActorEntrySpotAsync(
                RoutingId.From("entry-node"),
                actor,
                ZLinkMessage.Empty,
                CancellationToken.None);

            Assert.IsType<ZLinkActorJoinResult.Accepted>(result);
            Assert.Equal(0, Volatile.Read(ref attempts));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task LocalEntrySpotJoin_PreExistingNativeActor_IsNeverDestroyed()
    {
        var node = new CapturingSpotNode();
        ConfigureNotConnectedEntryJoin(node);
        var existing = new ZLinkBackendActorRef(RoutingId.From("entry-node"), "actor-a", 7);
        node.ActorLookupResult = existing;
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            entrySpotType: typeof(LocalJoinProbeEntrySpot));
        runtime.Services.GetRequiredService<LocalEntryJoinProbe>().Handler = _ =>
            ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);

            var result = await runtime.JoinActorEntrySpotAsync(
                RoutingId.From("entry-node"),
                actor,
                ZLinkMessage.Empty,
                CancellationToken.None);

            var accepted = Assert.IsType<ZLinkActorJoinResult.Accepted>(result);
            Assert.Equal(existing.ToNative("entry"), accepted.Actor);
            Assert.Empty(node.CreatedActors);
            Assert.Empty(node.DestroyedActors);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task NativeEntrySpotJoin_LateReplyAfterCancellation_IsDisposed()
    {
        var node = new CapturingSpotNode { DeferEntrySpotJoinCallback = true };
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        using var cancellation = new CancellationTokenSource();
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var join = runtime.JoinActorEntrySpotAsync(
                    RoutingId.From("remote-node"),
                    actor,
                    ZLinkMessage.Empty,
                    cancellation.Token)
                .AsTask();
            Assert.True(SpinWait.SpinUntil(
                () => node.DeferredEntrySpotJoinCallback is not null,
                TimeSpan.FromSeconds(5)));

            cancellation.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => join);

            using var lateReply = Message.From("late-entry-reply");
            node.DeferredEntrySpotJoinCallback!(
                new ZLinkBackendActorJoinEntrySpotResult(
                    RequestResult.Ok,
                    0,
                    actorRef,
                    RoutingId.From("remote-node"),
                    "entry-spot",
                    0,
                    0),
                [lateReply]);
            Assert.Throws<ObjectDisposedException>(() => lateReply.AsReadOnlySpan());
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task NativeUserSpotJoin_LateReplyAfterCancellation_IsDisposed()
    {
        var node = new CapturingSpotNode { DeferActorJoinCallback = true };
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node, includeJoinTarget: true);
        using var cancellation = new CancellationTokenSource();
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var target = await runtime.CreateAsync<JoinTargetSpot>();
            var activation = GetSpotActivation(runtime, target.Spot.SpotId);
            var joiner = new ZLinkNativeActorJoinOperation(
                runtime,
                runtime.Registration,
                runtime.GetOrCreateActorState);
            var join = joiner.JoinAsync(
                    actor,
                    actorRef,
                    node,
                    activation.NodeRid,
                    activation.SpotId,
                    activation.ChannelName,
                    ZLinkMessage.Empty,
                    cancellation.Token)
                .AsTask();
            Assert.True(SpinWait.SpinUntil(
                () => node.DeferredActorJoinCallback is not null,
                TimeSpan.FromSeconds(5)));

            cancellation.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => join);

            using var lateReply = Message.From("late-user-spot-reply");
            node.DeferredActorJoinCallback!(
                new ZLinkBackendActorJoinResult(
                    RequestResult.Ok,
                    0,
                    actorRef,
                    target.Spot.SpotId,
                    0,
                    0),
                [lateReply]);
            Assert.Throws<ObjectDisposedException>(() => lateReply.AsReadOnlySpan());
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ActorJoinPrewarmIngress_ParksArrivalBetweenAcceptedAndPrepare_ThenMigratesOnPrepare()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        try
        {
            const string actorId = "prewarm-actor";
            const string handoffId = "prewarm-handoff-1";
            const ulong actorGeneration = 4;
            var sourceNodeRid = RoutingId.From("prewarm-source");
            var repliedWithError = false;
            Func<IReadOnlyList<Message>, SendFlags, SubmitResult> directReply =
                (messages, _) =>
                {
                    repliedWithError = true;
                    foreach (var message in messages) message.Dispose();
                    return SubmitResult.Ok;
                };

            //  Step 1 (spec 15 §4.2 step 2): OnActorJoin admits and
            //  registers the relocation temporary queue before Accepted
            //  returns to the source — the exact call the production
            //  admission path makes
            //  (ZLinkFrameworkRuntimeActors.AdmitRoutedActorJoinAsync).
            await runtime.RegisterActorJoinPrewarmAsync(
                handoffId,
                actorId,
                actorGeneration,
                DateTimeOffset.UtcNow.AddSeconds(30),
                CancellationToken.None);

            //  Step 2: an arrival for this exact Actor lands on
            //  production ingress before PREPARE (Restore) has installed
            //  the real import. Without the prewarm registry this is
            //  dropped or replied NotFound.
            //  ZLinkActorInboundPipeline.DispatchAsync is the same
            //  production ingress entry point real inbound traffic uses.
            var actorRef = new ZLinkBackendActorRef(
                RoutingId.From("prewarm-target"), actorId, actorGeneration);
            var earlyFrame = CreateRelocationRequestFrame(
                actorRef, sourceNodeRid, requestId: 501, directReply);
            var pipeline = new ZLinkActorInboundPipeline(
                runtime, new ZLinkEntrySpotActorInboundEndpoint(runtime));
            await pipeline.DispatchAsync(
                new ZLinkSpotActorFrameBatch([earlyFrame]),
                CancellationToken.None);

            Assert.False(
                repliedWithError,
                "production ingress must park an arrival between Accepted and PREPARE instead of replying NotFound");

            //  Step 3 (spec 15 §4.2 step 4): PREPARE (Restore) installs
            //  the real import and must atomically migrate the parked
            //  arrival into it — the same calls
            //  ZLinkFrameworkRuntimeActors.PrepareCanonicalRoutedActorJoinTargetAsync
            //  makes.
            var actorState = runtime.GetOrCreateActorState(actorId);
            var request = PrewarmJoinRequest(handoffId, actorId, actorGeneration);
            Assert.True(actorState.Handoff.Import(request, out _));
            runtime.CompleteActorJoinPrewarmMigration(handoffId, actorState);

            var replay = actorState.Handoff.PrepareImportedReplay([]);
            Assert.Contains(
                replay,
                frame => Encoding.UTF8.GetString(frame.Body) == "request");
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ActorJoinPrewarmSupersession_DoesNotWaitForUnrelatedTargetAttemptGate()
    {
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        object? targetAttemptLease = null;
        Task? targetAttemptRun = null;
        TaskCompletionSource? releaseTargetAttempt = null;
        try
        {
            var owner = runtime.StandaloneActorRelocationRuntime;
            var ownerType = owner.GetType();
            var keyType = ownerType.GetNestedType(
                              "AttemptKey",
                              BindingFlags.NonPublic)
                          ?? throw new InvalidOperationException(
                              "Target attempt key was not found.");
            var key = Activator.CreateInstance(
                          keyType,
                          BindingFlags.Instance
                          | BindingFlags.Public
                          | BindingFlags.NonPublic,
                          binder: null,
                          args: [11UL, 12UL, 1UL],
                          culture: null)
                      ?? throw new InvalidOperationException(
                          "Target attempt key could not be created.");
            var acquire = ownerType.GetMethod(
                              "AcquireTargetAttempt",
                              BindingFlags.Instance | BindingFlags.NonPublic,
                              binder: null,
                              types: [keyType],
                              modifiers: null)
                          ?? throw new InvalidOperationException(
                              "Target attempt acquisition owner was not found.");
            targetAttemptLease = acquire.Invoke(owner, [key])
                                 ?? throw new InvalidOperationException(
                                     "Target attempt lease was not acquired.");
            var slot = targetAttemptLease.GetType()
                           .GetProperty(
                               "Slot",
                               BindingFlags.Instance | BindingFlags.NonPublic)!
                           .GetValue(targetAttemptLease)
                       ?? throw new InvalidOperationException(
                           "Target attempt slot was not found.");
            var runAsync = slot.GetType().GetMethod(
                               "RunAsync",
                               BindingFlags.Instance | BindingFlags.NonPublic,
                               binder: null,
                               types: [typeof(Func<ValueTask>)],
                               modifiers: null)
                           ?? throw new InvalidOperationException(
                               "Target attempt state-lane runner was not found.");
            var targetAttemptStarted = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            releaseTargetAttempt = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            Func<ValueTask> holdTargetAttempt = () =>
            {
                targetAttemptStarted.TrySetResult();
                return new ValueTask(releaseTargetAttempt.Task);
            };
            targetAttemptRun = ((ValueTask)(runAsync.Invoke(
                slot,
                [holdTargetAttempt])
                ?? throw new InvalidOperationException(
                    "Target attempt state-lane work was not scheduled."))).AsTask();
            await targetAttemptStarted.Task.WaitAsync(
                TimeSpan.FromSeconds(1));

            using var deadline = new CancellationTokenSource(
                TimeSpan.FromMilliseconds(100));
            await owner.AbortSupersededRoutedActorJoinPreparationAsync(
                "different-actor",
                actorGeneration: 42,
                newerHandoffId: "newer-handoff",
                deadline.Token);
        }
        finally
        {
            releaseTargetAttempt?.TrySetResult();
            if (targetAttemptRun is not null)
                await targetAttemptRun;
            (targetAttemptLease as IDisposable)?.Dispose();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ActorJoinPrewarmIngress_WithoutRegistration_ArrivalIsTreatedAsNotFound()
    {
        //  Disable-check: the same arrival, dispatched to the same
        //  not-yet-existing ActorId but with no RegisterActorJoinPrewarm
        //  call first — as if the prewarm mechanism were absent —
        //  reproduces the pre-fix behavior. This is what makes the
        //  companion test above meaningful: removing the registration
        //  call flips this assertion, so the mechanism is what the
        //  passing test above actually exercises.
        var node = new CapturingSpotNode();
        var (runtime, _) = await CreateStartedRuntimeAsync(node);
        try
        {
            const string actorId = "prewarm-actor-disabled";
            const ulong actorGeneration = 4;
            var sourceNodeRid = RoutingId.From("prewarm-source");
            var repliedWithError = false;
            Func<IReadOnlyList<Message>, SendFlags, SubmitResult> directReply =
                (messages, _) =>
                {
                    repliedWithError = true;
                    foreach (var message in messages) message.Dispose();
                    return SubmitResult.Ok;
                };
            var actorRef = new ZLinkBackendActorRef(
                RoutingId.From("prewarm-target"), actorId, actorGeneration);
            var frame = CreateRelocationRequestFrame(
                actorRef, sourceNodeRid, requestId: 502, directReply);
            var pipeline = new ZLinkActorInboundPipeline(
                runtime, new ZLinkEntrySpotActorInboundEndpoint(runtime));

            await pipeline.DispatchAsync(
                new ZLinkSpotActorFrameBatch([frame]),
                CancellationToken.None);

            Assert.True(
                repliedWithError,
                "without a prewarm registration the arrival must fall back to the pre-fix NotFound/stale reply");
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private static ZLinkRemoteActorJoinRequest PrewarmJoinRequest(
        string handoffId,
        string actorId,
        ulong actorGeneration)
        => new(
            actorId,
            "warrior",
            handoffId,
            null,
            null,
            "application/json",
            "root-1",
            7,
            Guid.Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
            1,
            new byte[32],
            "application/json",
            [],
            [],
            "source-spot",
            [2],
            actorGeneration,
            1);

    [Fact]
    public async Task ActorRemoteJoiner_InternalDeadline_MapsToTypedDeadlineExceeded()
    {
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await ZLinkActorRemoteJoiner.ExecuteWithDeadlineAsync(
                async cancellationToken =>
                {
                    await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
                    return 0;
                },
                TimeSpan.FromMilliseconds(20),
                CancellationToken.None));

        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
        Assert.IsAssignableFrom<OperationCanceledException>(error.InnerException);
    }

    [Fact]
    public async Task NativeActorJoinCompletion_NormalWinnerRetainsReplyOwnershipForDecoder()
    {
        using var completion = new ZLinkNativeReplyCompletion<int>(CancellationToken.None);
        using var reply = Message.From("normal-reply");

        completion.Complete(7, [reply]);
        var result = await completion.Task;

        Assert.Equal(7, result.Result);
        Assert.Same(reply, Assert.Single(result.Reply));
        Assert.Equal("normal-reply", System.Text.Encoding.UTF8.GetString(reply.AsReadOnlySpan()));
    }

    [Fact]
    public async Task NativeUserSpotJoin_BackendThrow_DisposesRequestPartsAndPreservesException()
    {
        var failure = new InvalidOperationException("native join submit failed");
        var node = new CapturingSpotNode { ActorJoinSubmitFailure = failure };
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node, includeJoinTarget: true);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var target = await runtime.CreateAsync<JoinTargetSpot>();
            var activation = GetSpotActivation(runtime, target.Spot.SpotId);
            var joiner = new ZLinkNativeActorJoinOperation(
                runtime,
                runtime.Registration,
                runtime.GetOrCreateActorState);

            var thrown = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
                await joiner.JoinAsync(
                    actor,
                    actorRef,
                    node,
                    activation.NodeRid,
                    activation.SpotId,
                    activation.ChannelName,
                    ZLinkMessage.Empty,
                    CancellationToken.None));

            Assert.Same(failure, thrown);
            var submitted = Assert.IsAssignableFrom<IReadOnlyList<Message>>(node.ActorJoinSubmittedParts);
            Assert.NotEmpty(submitted);
            Assert.All(submitted, part =>
                Assert.Throws<ObjectDisposedException>(() => part.AsReadOnlySpan()));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task NativeUserSpotJoin_SubmitRejection_MapsNotFoundAndDisposesRequestParts()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            includeJoinTarget: true);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var target = await runtime.CreateAsync<JoinTargetSpot>();
            var activation = GetSpotActivation(runtime, target.Spot.SpotId);
            var joiner = new ZLinkNativeActorJoinOperation(
                runtime,
                runtime.Registration,
                runtime.GetOrCreateActorState);

            var thrown = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await joiner.JoinAsync(
                    actor,
                    actorRef,
                    node,
                    activation.NodeRid,
                    activation.SpotId,
                    activation.ChannelName,
                    ZLinkMessage.Empty,
                    CancellationToken.None));

            Assert.Equal(ZLinkFrameworkErrorKind.NotFound, thrown.Kind);
            var submitted = Assert.IsAssignableFrom<IReadOnlyList<Message>>(
                node.ActorJoinSubmittedParts);
            Assert.NotEmpty(submitted);
            Assert.All(submitted, part =>
                Assert.Throws<ObjectDisposedException>(() => part.AsReadOnlySpan()));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task NativeActorJoin_Records_Target_Node_As_SourceRid_Without_PeerRid()
    {
        var observer = new CapturingMessageFlowObserver();
        var node = new CapturingSpotNode();
        ZLinkEnvelopeHeader? requestHeader = null;
        ZLinkEnvelopeHeader? replyHeader = null;
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            observer,
            includeJoinTarget: true);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var created = await runtime.CreateAsync<JoinTargetSpot>();
            node.ActorJoinHandler = parts =>
            {
                var decodedRequestHeader = ZLinkEnvelopeCodec.DecodeHeader(parts);
                requestHeader = decodedRequestHeader;
                var targetActor = new ZLinkBackendActorRef(
                    RoutingId.From("joined-node"),
                    actor.ActorId,
                    actorRef.Generation);
                var result = new ZLinkBackendActorJoinResult(
                    RequestResult.Ok,
                    0,
                    targetActor,
                    created.Spot.SpotId,
                    1,
                    0);
                var reply = ZLinkSpotReplyEnvelope.EncodeActorJoinReplyParts(
                    "entry-channel",
                    decodedRequestHeader.MessageName,
                    ZLinkMessage.From("accepted"),
                    typeof(ZLinkMessage),
                    runtime.Registration.Codecs);
                replyHeader = ZLinkEnvelopeCodec.DecodeHeader(reply);
                return (result, reply);
            };
            var activation = GetSpotActivation(runtime, created.Spot.SpotId);
            var joiner = new ZLinkNativeActorJoinOperation(
                runtime,
                runtime.Registration,
                runtime.GetOrCreateActorState);

            Assert.Null(ZLinkFlowContext.Current);
            var result = await joiner.JoinAsync(
                actor,
                actorRef,
                node,
                activation.NodeRid,
                activation.SpotId,
                activation.ChannelName,
                ZLinkMessage.From("join"),
                CancellationToken.None);

            Assert.IsType<ZLinkActorJoinResult.Accepted>(result);
            Assert.Null(ZLinkFlowContext.Current);
            var capturedRequestHeader = Assert.IsType<ZLinkEnvelopeHeader>(requestHeader);
            var capturedReplyHeader = Assert.IsType<ZLinkEnvelopeHeader>(replyHeader);
            Assert.Equal(ZLinkMessageKind.Command, capturedRequestHeader.Kind);
            Assert.Equal(ZLinkMessageKind.Command, capturedReplyHeader.Kind);
            Assert.True(ZlinkStreamFlowId.IsValid(capturedRequestHeader.FlowId));
            Assert.Equal(ZLinkFlowOrigin.Application, capturedRequestHeader.FlowOrigin);
            Assert.Equal(capturedRequestHeader.FlowId, capturedReplyHeader.FlowId);
            Assert.Equal(capturedRequestHeader.FlowOrigin, capturedReplyHeader.FlowOrigin);
            Assert.Null(capturedRequestHeader.CorrelationId);
            Assert.Null(capturedReplyHeader.CorrelationId);
            for (var attempt = 0; attempt < 100
                                  && observer.Events.Count(flow => flow.Phase is
                                      "sent" or "reply_received") < 2;
                 attempt++)
                await Task.Delay(5);
            var flowEvents = observer.Events
                .Where(flow => flow.Phase is "sent" or "reply_received")
                .ToArray();
            Assert.Equal(2, flowEvents.Length);
            Assert.Equal(
                ["sent", "reply_received"],
                flowEvents.Select(flow => flow.Phase));
            Assert.All(flowEvents, flow =>
            {
                Assert.Equal(node.LastActorJoinTargetNodeRid.ToString(), flow.SourceRid);
                Assert.Null(flow.TargetRid);
                Assert.Equal(created.Spot.SpotId.ToString(), flow.SpotId);
                Assert.Equal(actor.ActorId, flow.ActorId);
                Assert.Equal(capturedRequestHeader.FlowId, flow.FlowId);
                Assert.Equal("application", flow.FlowOrigin);
                Assert.Null(flow.CorrelationId);
            });
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ActorJoinDispatcher_Preserves_Wire_Flow_Through_Handler_And_Reply()
    {
        const string flowId = "0196f7c2-4cb4-7cc8-89d4-2d6aee6fca2d";
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var spot = new ActorJoinFlowProbeSpot();
            var actorJoins = new ZLinkSpotActorJoinRegistry();
            actorJoins.Bind(spot);
            var actors = new ZLinkSpotActorMembership();
            actors.Add(actor);
            await using var handlerInstances = new ZLinkScopedHandlerInstanceOwner(runtime.Services);
            var invoker = new ZLinkSpotHandlerInvoker(
                handlerInstances,
                spot,
                runtime.Registration.Codecs,
                ZLinkStreamProtocolDefaults.CreateLz4CompressionCodec());
            var nativeSpot = new CapturingSpot();
            var dispatcher = new ZLinkSpotActorJoinDispatcher(
                runtime,
                nativeSpot,
                "join-channel",
                actorJoins,
                actors,
                () => invoker);
            var requestHeader = new ZLinkEnvelopeHeader(
                ZLinkMessageKind.Command,
                "join-channel",
                typeof(ZLinkMessage).Name,
                ZLinkEnvelopeCodec.DefaultContentType,
                null,
                null,
                null,
                null,
                null)
            {
                FlowId = flowId,
                FlowOrigin = ZLinkFlowOrigin.Application
            };
            var parts = ZLinkEnvelopeCodec.EncodeParts(
                requestHeader,
                ZLinkMessage.From("join-request"),
                typeof(ZLinkMessage),
                runtime.Registration.Codecs);
            var request = new ZLinkBackendActorJoinRequest(
                actorRef,
                actorRef,
                RoutingId.From("source-node"),
                "target-spot",
                1,
                parts[0],
                parts);

            Assert.Null(ZLinkFlowContext.Current);
            try
            {
                await dispatcher.DispatchAsync(request, CancellationToken.None);
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(parts);
            }

            Assert.Null(ZLinkFlowContext.Current);
            Assert.Equal(flowId, spot.ObservedFlowId);
            Assert.Equal(ZLinkFlowOrigin.Application, spot.ObservedFlowOrigin);
            Assert.Equal("join-request", spot.ObservedRequest);
            Assert.Equal(0, nativeSpot.ActorJoinResultCode);
            var replyHeader = Assert.IsType<ZLinkEnvelopeHeader>(nativeSpot.ActorJoinReplyHeader);
            Assert.Equal(ZLinkMessageKind.Command, replyHeader.Kind);
            Assert.Equal(flowId, replyHeader.FlowId);
            Assert.Equal(ZLinkFlowOrigin.Application, replyHeader.FlowOrigin);
            Assert.Null(replyHeader.CorrelationId);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ReplyCanonicalAdmission_SendsSoleRawPart_NoNestedEnvelope()
    {
        // spec 51 (c56714a52c): the actorJoin(28) reply carries the
        // application reply as a sole raw part — the target must never wrap
        // it in an additional ZLinkApplicationPayloadEnvelopeCodec envelope
        // (the removed .NET-only nested-envelope dialect).
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var spot = new ActorJoinFlowProbeSpot();
            var actorJoins = new ZLinkSpotActorJoinRegistry();
            actorJoins.Bind(spot);
            var actors = new ZLinkSpotActorMembership();
            actors.Add(actor);
            await using var handlerInstances = new ZLinkScopedHandlerInstanceOwner(runtime.Services);
            var invoker = new ZLinkSpotHandlerInvoker(
                handlerInstances,
                spot,
                runtime.Registration.Codecs,
                ZLinkStreamProtocolDefaults.CreateLz4CompressionCodec());
            var nativeSpot = new CapturingSpot();
            var dispatcher = new ZLinkSpotActorJoinDispatcher(
                runtime,
                nativeSpot,
                "join-channel",
                actorJoins,
                actors,
                () => invoker);

            using var emptyRequestPart = Message.From(ReadOnlySpan<byte>.Empty);
            var joinRequest = new ZLinkBackendActorJoinRequest(
                actorRef,
                actorRef,
                RoutingId.From("source-node"),
                "target-spot",
                1,
                emptyRequestPart,
                [emptyRequestPart]);
            var rawReplyBytes = "{\"accepted\":true}"u8.ToArray();
            var admission = new ZLinkRemoteActorAdmissionReply(
                Accepted: true,
                ReplyContentType: "application/json",
                Reply: rawReplyBytes);

            var method = typeof(ZLinkSpotActorJoinDispatcher).GetMethod(
                "ReplyCanonicalAdmission",
                BindingFlags.NonPublic | BindingFlags.Instance)
                ?? throw new InvalidOperationException(
                    "ReplyCanonicalAdmission was not found by reflection.");
            method.Invoke(dispatcher, [joinRequest, admission]);

            Assert.Equal(0, nativeSpot.ActorJoinResultCode);
            Assert.Equal(rawReplyBytes, nativeSpot.ActorJoinReplyMessageBytes);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task DeferredJoin_UsesTheCapturedActorGeneration_WhenRegistryPublishesASuccessor()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            var registry = new ZLinkActorSessionRegistry(runtime.Services);
            var actorId = ZLinkActorId.FromBoundary(
                actorRef.ActorId,
                "actorId");
            var sourceState = registry.GetOrCreate(actorId);
            sourceState.BindNativeActorRef(actorRef);
            var context = sourceState.GetOrCreateContext(() =>
                new ZLinkActorContext(
                    runtime,
                    sourceState,
                    "entry",
                    actorRef.Generation,
                    spotId: null,
                    boundSessionService: new ZLinkBoundSessionService(runtime)));
            var actor = new ProbeActor(actorRef.ActorId, context);
            Assert.True(sourceState.BindActorInstance(actor));
            var join = context.JoinSpot(
                "successor-race-target",
                ZLinkMessage.Empty);

            // Reproduce the reset window after the old context validation:
            // the registry can publish a successor before the old state is fenced.
            registry.RemoveIfCurrent(actorId, sourceState);
            var successor = registry.GetOrCreate(actorId);
            successor.BindNativeActorRef(
                actorRef with { Generation = actorRef.Generation + 1 });
            Assert.NotSame(sourceState, successor);

            using (var handler = ZLinkDeferredActorJoinHandlerScope.Open())
            {
                join.Defer();

                Assert.True(sourceState.Handoff.CapturesSourceIngress);
                Assert.False(successor.Handoff.CapturesSourceIngress);
            }

            Assert.False(sourceState.Handoff.CapturesSourceIngress);
            Assert.False(successor.Handoff.CapturesSourceIngress);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task DeferredJoin_FromBoundSessionRequest_IsScheduledAfterReplyBoundary()
    {
        var node = new CapturingSpotNode();
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(node);
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var sourceState = runtime.GetOrCreateActorState(actor.ActorId);
            var join = actor.Context.JoinSpot(
                "missing-bound-session-target",
                ZLinkMessage.Empty);

            await using var boundSession =
                ZLinkBoundSessionDispatchScope.Enter(actor.ActorId);
            using (var handler = ZLinkDeferredActorJoinHandlerScope.Open())
            {
                join.Defer();
                handler.Complete();
            }

            // Handler completion only registers work at this point. The
            // bound-session reply boundary drains before relocation starts.
            Assert.True(sourceState.Handoff.CapturesSourceIngress);

            await boundSession.DrainAsync(CancellationToken.None);

            Assert.True(SpinWait.SpinUntil(
                () => !sourceState.Handoff.CapturesSourceIngress,
                TimeSpan.FromSeconds(5)));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private static ZLinkSpotActivation GetSpotActivation(
        ZLinkFrameworkRuntime runtime,
        string spotId) =>
        Assert.Single(
            runtime.GetSpotNodeRuntime("entry").Catalog.Spots,
            candidate => candidate.SpotId == spotId);

    private static async ValueTask CreateTrackedActorOwnershipAsync(
        IZLinkLocationRepository store,
        string ownerId,
        ZLinkActorOwnershipCoordinator ownership,
        string actorType,
        string actorId,
        RoutingId nodeRid,
        CancellationToken cancellationToken,
        string meshName = "play",
        ulong objectGeneration = 1)
    {
        var authority = InMemoryLocationStoreTests.Actor(ownerId, actorId) with
        {
            ActorType = actorType,
            MeshName = meshName,
            ActorRef = new ActorRef(actorId, objectGeneration, meshName, nodeRid),
            OwnerNodeRid = nodeRid,
            OwnerNodeGeneration = 1
        };
        Assert.IsType<ZLinkAuthoritySnapshot>(
            await AuthorityLocationTestFixture.PublishActorAsync(store, authority));
        var activation = await ownership.ExecuteActorClaimThenActivateAsync(
            ZLinkMeshName.FromBoundary(meshName, nameof(meshName)),
            actorType,
            ZLinkActorId.FromBoundary(actorId, nameof(actorId)),
            nodeRid,
            deactivate: null,
            activate: static _ => ValueTask.FromResult(new object()),
            cancellationToken: cancellationToken);
        Assert.NotNull(activation.Activated);
        Assert.Null(activation.ExistingLocation);
    }

    private static async ValueTask CreateTrackedLocalActorOwnershipAsync(
        IZLinkLocationRepository store,
        ZLinkLocationOwnerToken owner,
        ZLinkActorOwnershipCoordinator ownership,
        ZLinkBackendActorRef actor,
        CancellationToken cancellationToken)
    {
        const string meshName = "entry";
        const string stableType = "probe";
        var payload = ZLinkActorAuthorityPayloadCodec.Encode(
            new ZLinkActorAuthorityPayload(
                ZLinkActorAuthorityState.Ready,
                stableType,
                actor.ActorId,
                "entry:test",
                1,
                ZLinkSpotKind.Entry,
                owner.OwnerId,
                checked((ulong)owner.LeaseGeneration),
                meshName,
                actor.NodeRid,
                1));
        await ReserveAndCommitLocalAuthorityAsync(
            store,
            ZLinkActorAuthorityPayloadCodec.AuthorityKey(actor.ActorId),
            ZLinkPlacementObjectKind.Actor,
            stableType,
            actor.NodeRid,
            owner,
            payload,
            new ZLinkCapacityVector(1, 0, null));
        var activation = await ownership.ExecuteActorClaimThenActivateAsync(
            ZLinkMeshName.FromBoundary(meshName, nameof(meshName)),
            stableType,
            ZLinkActorId.FromBoundary(actor.ActorId, nameof(actor.ActorId)),
            actor.NodeRid,
            deactivate: null,
            activate: static _ => ValueTask.FromResult(new object()),
            cancellationToken: cancellationToken);
        Assert.NotNull(activation.Activated);
        Assert.Null(activation.ExistingLocation);
    }

    private static async ValueTask ReserveAndCommitLocalAuthorityAsync(
        IZLinkLocationRepository store,
        ZLinkAuthorityKey key,
        ZLinkPlacementObjectKind objectKind,
        string stableType,
        RoutingId nodeRid,
        ZLinkLocationOwnerToken owner,
        byte[] payload,
        ZLinkCapacityVector capacity)
    {
        var intent = Encoding.UTF8.GetBytes($"create:{key.Value}");
        var reserve = await store.ReserveAsync(
                new ZLinkObjectReservationRequest(
                    objectKind,
                    key,
                    stableType,
                    $"inline:{key.Value}",
                    System.Security.Cryptography.SHA256.HashData(intent),
                    intent.Length,
                    new ZLinkMeshNodeDescriptorKey("entry", nodeRid),
                    1,
                    owner,
                    payload,
                    capacity));
        if (reserve is not ZLinkObjectReserveResult.Reserved reserved)
        {
            var descriptor = (await store.ListMeshNodesAsync("entry", default))
                .Items
                .Single(item => item.Rid == nodeRid);
            throw new InvalidOperationException(
                $"Reserve failed with {reserve.GetType().Name}; "
                + $"owner={owner.OwnerId}/{owner.LeaseGeneration}, "
                + $"descriptor={descriptor.OwnerId}/{descriptor.LeaseGeneration}, "
                + $"lifecycle={descriptor.LifecycleGeneration}, "
                + $"role={descriptor.ObjectRole}, state={descriptor.State}, weight={descriptor.PlacementWeight}, "
                + $"actor-capacity={descriptor.Capacity.Actors}, "
                + $"capabilities={string.Join(',', descriptor.ObjectCapabilities.Select(static value => $"{value.ObjectKind}:{value.StableType}"))}.");
        }
        Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(reserved.Reservation, payload));
    }

    [Fact]
    public async Task RelocationReplyCapability_CapturedRequestCompletesExactlyOnceAfterLocalReplay()
    {
        var node = new CapturingSpotNode();
        var (runtime, actor) = await CreateStartedRuntimeAsync(node);
        try
        {
            var sourceNodeRid = RoutingId.From("request-source");
            var replies = 0;
            using var frame = CreateRelocationRequestFrame(
                actor,
                sourceNodeRid,
                requestId: 701,
                (_, _) =>
                {
                    Interlocked.Increment(ref replies);
                    return SubmitResult.Ok;
                });
            var handoff = new ZLinkActorHandoffState(
                actor.ActorId,
                TimeProvider.System);
            handoff.BeginCapture();

            Assert.Equal(
                ZLinkActorHandoffCaptureResult.Captured,
                handoff.TryCapture(
                    frame,
                    runtime,
                    static (rt, fr) =>
                        ZLinkActorInboundPipeline.EnsureRelocationReplyRoute(rt, fr)));
            var captured = Assert.Single(handoff.SnapshotFrames());
            Assert.False(string.IsNullOrWhiteSpace(
                captured.RouteContext.ReplyCapability));
            Assert.True(runtime.ActorMessageFollower.TryResolveReplyRoute(
                captured.RouteContext.ReplyCapability!,
                out var replyNodeRid,
                out var replyDeadlineUnixMs));
            Assert.Equal(actor.NodeRid, replyNodeRid);
            Assert.Equal(
                frame.RouteContext.DeadlineUnixMs,
                replyDeadlineUnixMs);
            Assert.False(runtime.ActorMessageFollower.TryResolveReplyRoute(
                $"v2.{actor.NodeRid.ToHex()}.{ulong.MaxValue}.invalid",
                out _,
                out _));

            using (var reply = Message.From("first"))
                await runtime.ReplyActorNoBindAsync(
                    actor,
                    sourceNodeRid,
                    default,
                    701,
                    ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                    captured.RouteContext.ReplyCapability,
                    [reply]);
            using (var duplicate = Message.From("duplicate"))
                await runtime.ReplyActorNoBindAsync(
                    actor,
                    sourceNodeRid,
                    default,
                    701,
                    ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                    captured.RouteContext.ReplyCapability,
                    [duplicate]);

            Assert.Equal(1, Volatile.Read(ref replies));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task RelocationReplyCapability_MessageFollowKeepsTheCapturedCapability()
    {
        var node = new CapturingSpotNode();
        var (runtime, actor) = await CreateStartedRuntimeAsync(node);
        try
        {
            node.NodeSendAttempts.Clear();
            var sourceNodeRid = RoutingId.From("request-source");
            using var frame = CreateRelocationRequestFrame(
                actor,
                sourceNodeRid,
                requestId: 711,
                static (_, _) => SubmitResult.Ok);
            ZLinkActorInboundPipeline.EnsureRelocationReplyRoute(
                runtime,
                frame);
            var capturedCapability = Assert.IsType<string>(
                frame.RouteContext.ReplyCapability);
            var target = new ZLinkBackendActorRef(
                RoutingId.From("remote-owner"),
                actor.ActorId,
                actor.Generation);
            var lease = new ZLinkActorMessageFollowLease(TimeProvider.System);
            lease.Commit(TimeSpan.FromSeconds(5));
            var route = new ZLinkActorMessageFollowRoute(
                actor,
                target,
                "entry",
                SourceNodeGeneration: 13,
                TargetNodeGeneration: 23,
                SourceAuthorityOwnerGeneration: 17,
                TargetAuthorityOwnerGeneration: 27,
                SourceOwnerLeaseGeneration: 19,
                TargetOwnerLeaseGeneration: 29,
                Lease: lease);

            runtime.ActorMessageFollower.Enqueue(
                route,
                frame.SourceNodeRid,
                frame.SourceSessionRid,
                frame.RequestId,
                frame.Flags,
                frame.RouteContext,
                frame.Header,
                frame.Body,
                frame.SourceNodeGeneration,
                frame.RequestSource,
                frame.DirectReply);

            Assert.True(SpinWait.SpinUntil(
                () => node.NodeSendAttempts.Count == 1,
                TimeSpan.FromSeconds(5)));
            var relay = ZLinkFrameworkJsonPayloadCodec
                .Deserialize<ZLinkRemoteActorFrameRelay>(
                    node.NodeSendAttempts.Single()[1]);
            Assert.NotNull(relay);
            Assert.Equal(capturedCapability, relay.ReplyCapability);
            Assert.Equal(frame.RouteContext.OperationId.High, relay.OperationIdHigh);
            Assert.Equal(frame.RouteContext.OperationId.Low, relay.OperationIdLow);
            Assert.Equal(frame.RelocationReplyRouteId, relay.ReplyRequestId);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task RelocationReplyCapability_FullCaptureUsesLocalTerminalAndReleasesRoute()
    {
        var (runtime, actor) = await CreateStartedRuntimeAsync(new CapturingSpotNode());
        try
        {
            var sourceNodeRid = RoutingId.From("request-source");
            var replies = 0;
            using var frame = CreateRelocationRequestFrame(
                actor,
                sourceNodeRid,
                requestId: 702,
                (_, _) =>
                {
                    Interlocked.Increment(ref replies);
                    return SubmitResult.Ok;
                });
            var handoff = new ZLinkActorHandoffState(
                actor.ActorId,
                TimeProvider.System,
                sourceIngressAdmission: new ZLinkBoundedIngressAdmission(
                    recordCapacity: 1,
                    byteCapacity: 1));
            handoff.BeginCapture();

            Assert.Equal(
                ZLinkActorHandoffCaptureResult.Full,
                handoff.TryCapture(
                    frame,
                    runtime,
                    static (rt, fr) =>
                        ZLinkActorInboundPipeline.EnsureRelocationReplyRoute(rt, fr)));
            Assert.NotNull(frame.DirectReply);
            using var first = Message.From("first");
            Assert.Equal(
                SubmitResult.Ok,
                frame.DirectReply!([first], SendFlags.DontWait));
            using var duplicate = Message.From("duplicate");
            Assert.Equal(
                SubmitResult.Terminated,
                frame.DirectReply!([duplicate], SendFlags.DontWait));
            Assert.Equal(1, Volatile.Read(ref replies));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task RelocationReplyCapability_ExpiresAtOriginalRequestDeadline()
    {
        var (runtime, actor) = await CreateStartedRuntimeAsync(new CapturingSpotNode());
        try
        {
            var replies = 0;
            var preserved = runtime.ActorMessageFollower.PreserveDirectReply(
                actor.NodeRid,
                actor.ActorId,
                requestId: 703,
                deadlineUnixMs: checked((ulong)DateTimeOffset.UtcNow
                    .AddMilliseconds(20)
                    .ToUnixTimeMilliseconds()),
                (_, _) =>
                {
                    Interlocked.Increment(ref replies);
                    return SubmitResult.Ok;
                });

            await Task.Delay(TimeSpan.FromMilliseconds(100));
            using var late = Message.From("late");
            Assert.Equal(
                SubmitResult.Terminated,
                preserved.Reply([late], SendFlags.DontWait));
            Assert.Equal(0, Volatile.Read(ref replies));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task MessageFollowDirectReply_BackpressureIsOneShotTerminal()
    {
        var (runtime, actor) = await CreateStartedRuntimeAsync(new CapturingSpotNode());
        try
        {
            var attempts = 0;
            var preserved = runtime.ActorMessageFollower.PreserveDirectReply(
                actor.NodeRid,
                actor.ActorId,
                requestId: 704,
                deadlineUnixMs: checked((ulong)DateTimeOffset.UtcNow
                    .AddSeconds(1)
                    .ToUnixTimeMilliseconds()),
                (_, _) =>
                {
                    Interlocked.Increment(ref attempts);
                    return SubmitResult.Backpressured;
                });

            Assert.True(await runtime.ActorMessageFollower.TryCompleteDirectReplyAsync(
                actor.ActorId,
                requestId: 704,
                ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                preserved.Capability,
                actor.NodeRid,
                actor.NodeRid,
                [1, 2, 3],
                CancellationToken.None));
            Assert.False(await runtime.ActorMessageFollower.TryCompleteDirectReplyAsync(
                actor.ActorId,
                requestId: 704,
                ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                preserved.Capability,
                actor.NodeRid,
                actor.NodeRid,
                [4],
                CancellationToken.None));
            Assert.Equal(1, Volatile.Read(ref attempts));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task MessageFollowDirectReply_ExpiredBeforeAttemptDoesNotInvokeCompletion()
    {
        var (runtime, actor) = await CreateStartedRuntimeAsync(new CapturingSpotNode());
        try
        {
            var attempts = 0;
            var preserved = runtime.ActorMessageFollower.PreserveDirectReply(
                actor.NodeRid,
                actor.ActorId,
                requestId: 705,
                deadlineUnixMs: checked((ulong)DateTimeOffset.UtcNow
                    .AddMilliseconds(-1)
                    .ToUnixTimeMilliseconds()),
                (_, _) =>
                {
                    Interlocked.Increment(ref attempts);
                    return SubmitResult.Backpressured;
                });

            Assert.True(await runtime.ActorMessageFollower.TryCompleteDirectReplyAsync(
                actor.ActorId,
                requestId: 705,
                ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                preserved.Capability,
                actor.NodeRid,
                actor.NodeRid,
                [1],
                CancellationToken.None));
            Assert.Equal(0, Volatile.Read(ref attempts));
            Assert.False(await runtime.ActorMessageFollower.TryCompleteDirectReplyAsync(
                actor.ActorId,
                requestId: 705,
                ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                preserved.Capability,
                actor.NodeRid,
                actor.NodeRid,
                [2],
                CancellationToken.None));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task MessageFollowDirectReply_ZeroDeadlineBackpressureIsOneShotTerminal()
    {
        var (runtime, actor) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            defaultRequestTimeout: TimeSpan.FromMilliseconds(40));
        try
        {
            var attempts = 0;
            var preserved = runtime.ActorMessageFollower.PreserveDirectReply(
                actor.NodeRid,
                actor.ActorId,
                requestId: 707,
                deadlineUnixMs: 0,
                (_, _) =>
                {
                    Interlocked.Increment(ref attempts);
                    return SubmitResult.Backpressured;
                });

            Assert.True(await runtime.ActorMessageFollower.TryCompleteDirectReplyAsync(
                actor.ActorId,
                requestId: 707,
                ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                preserved.Capability,
                actor.NodeRid,
                actor.NodeRid,
                [1],
                CancellationToken.None));
            Assert.Equal(1, Volatile.Read(ref attempts));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task MessageFollowDirectReply_PreCancelledAttemptReleasesClaim()
    {
        var (runtime, actor) = await CreateStartedRuntimeAsync(new CapturingSpotNode());
        try
        {
            var admitted = false;
            var successful = 0;
            var preserved = runtime.ActorMessageFollower.PreserveDirectReply(
                actor.NodeRid,
                actor.ActorId,
                requestId: 708,
                deadlineUnixMs: checked((ulong)DateTimeOffset.UtcNow
                    .AddSeconds(1)
                    .ToUnixTimeMilliseconds()),
                (_, _) =>
                {
                    if (!Volatile.Read(ref admitted))
                        return SubmitResult.Backpressured;
                    Interlocked.Increment(ref successful);
                    return SubmitResult.Ok;
                });
            using var cancelled = new CancellationTokenSource();
            cancelled.Cancel();

            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                () => runtime.ActorMessageFollower.TryCompleteDirectReplyAsync(
                        actor.ActorId,
                        requestId: 708,
                        ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                        preserved.Capability,
                        actor.NodeRid,
                        actor.NodeRid,
                        [1],
                        cancelled.Token)
                    .AsTask());

            Volatile.Write(ref admitted, true);
            Assert.True(await runtime.ActorMessageFollower.TryCompleteDirectReplyAsync(
                actor.ActorId,
                requestId: 708,
                ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                preserved.Capability,
                actor.NodeRid,
                actor.NodeRid,
                [2],
                CancellationToken.None));
            Assert.Equal(1, Volatile.Read(ref successful));
            Assert.False(await runtime.ActorMessageFollower.TryCompleteDirectReplyAsync(
                actor.ActorId,
                requestId: 708,
                ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                preserved.Capability,
                actor.NodeRid,
                actor.NodeRid,
                [3],
                CancellationToken.None));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task MessageFollowRemoteReplyRelay_UsesCanonicalNodeAsyncAdmission()
    {
        var node = new CapturingSpotNode();
        node.NodeSendResults.Enqueue(SubmitResult.Backpressured);
        node.NodeSendResults.Enqueue(SubmitResult.Ok);
        var (runtime, actor) = await CreateStartedRuntimeAsync(node);
        try
        {
            var remoteReplyNode = RoutingId.From("remote-reply-node");
            var preserved = runtime.ActorMessageFollower.PreserveDirectReply(
                remoteReplyNode,
                actor.ActorId,
                requestId: 706,
                deadlineUnixMs: checked((ulong)DateTimeOffset.UtcNow
                    .AddSeconds(1)
                    .ToUnixTimeMilliseconds()),
                static (_, _) => SubmitResult.Ok);
            using var reply = Message.From("reply");

            await runtime.ReplyActorNoBindAsync(
                actor,
                sourceNodeRid: remoteReplyNode,
                sourceSessionRid: default,
                requestId: 706,
                flags: ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                preserved.Capability,
                [reply]);

            Assert.Equal(1, node.NodeSendAsyncCalls);
            Assert.Equal(2, node.NodeSendAttempts.Count);
            Assert.All(
                node.NodeSendAttempts,
                static attempt => Assert.NotEmpty(attempt));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task MessageFollowLocalReply_BackpressureIsOneShotTerminal()
    {
        var (runtime, actor) = await CreateStartedRuntimeAsync(new CapturingSpotNode());
        try
        {
            var attempts = 0;
            var preserved = runtime.ActorMessageFollower.PreserveDirectReply(
                actor.NodeRid,
                actor.ActorId,
                requestId: 709,
                deadlineUnixMs: checked((ulong)DateTimeOffset.UtcNow
                    .AddSeconds(1)
                    .ToUnixTimeMilliseconds()),
                (_, _) =>
                {
                    Interlocked.Increment(ref attempts);
                    return SubmitResult.Backpressured;
                });
            using var reply = Message.From("local-reply");

            await runtime.ReplyActorNoBindAsync(
                actor,
                sourceNodeRid: actor.NodeRid,
                sourceSessionRid: default,
                requestId: 709,
                flags: ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                preserved.Capability,
                [reply]);

            Assert.Equal(1, Volatile.Read(ref attempts));
            using var duplicate = Message.From("duplicate");
            await runtime.ReplyActorNoBindAsync(
                actor,
                sourceNodeRid: actor.NodeRid,
                sourceSessionRid: default,
                requestId: 709,
                flags: ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                preserved.Capability,
                [duplicate]);
            Assert.Equal(1, Volatile.Read(ref attempts));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task MessageFollowRemoteReplyRelay_DoesNotSubmitAfterDeadline()
    {
        var node = new CapturingSpotNode();
        var (runtime, actor) = await CreateStartedRuntimeAsync(node);
        try
        {
            var remoteReplyNode = RoutingId.From("expired-reply-node");
            var preserved = runtime.ActorMessageFollower.PreserveDirectReply(
                remoteReplyNode,
                actor.ActorId,
                requestId: 710,
                deadlineUnixMs: checked((ulong)DateTimeOffset.UtcNow
                    .AddMilliseconds(100)
                    .ToUnixTimeMilliseconds()),
                static (_, _) => SubmitResult.Ok);
            await Task.Delay(TimeSpan.FromMilliseconds(300));
            using var reply = Message.From("late-reply");

            await runtime.ReplyActorNoBindAsync(
                actor,
                sourceNodeRid: remoteReplyNode,
                sourceSessionRid: default,
                requestId: 710,
                flags: ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                preserved.Capability,
                [reply]);

            Assert.Empty(node.NodeSendAttempts);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private static ZLinkSpotActorFrame CreateRelocationRequestFrame(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        ulong requestId,
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult> directReply)
    {
        return new ZLinkSpotActorFrame(
            actor,
            actor,
            sourceNodeRid,
            default,
            requestId,
            ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
            new ZLinkBackendActorRouteContext(
                new MeshOperationId(11, requestId),
                MessageFollowHopCount: 0,
                TargetNodeGeneration: 13,
                AuthorityOwnerGeneration: 17,
                OwnerLeaseGeneration: 19,
                ReplyRequestId: requestId,
                ReplyFlags: ZLinkActorBoundSessionRelay.ActorRecvInfoNoBind,
                DeadlineUnixMs: checked((ulong)DateTimeOffset.UtcNow
                    .AddSeconds(5)
                    .ToUnixTimeMilliseconds())),
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Request,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                new ZlinkStreamRequestSeq(requestId),
                $"relocation-reply-{requestId}",
                ZlinkStreamMetadata.Empty),
            Message.From("request"),
            sourceNodeGeneration: 23,
            new ZLinkServiceWireCodec.RequestSourceFence(
                "request-owner",
                29,
                sourceNodeRid,
                NodeGeneration: 23),
            directReply);
    }

    private static async Task DispatchAsync(
        ZLinkEntrySpotActivation activation,
        ZLinkSpotActorPacketDescriptor descriptor,
        ZLinkActorRuntimeState state,
        ProbeActor actor,
        string name)
    {
        var header = CreateHeader(name);
        using var body = Message.From(Encoding.UTF8.GetBytes(name));
        await state.ExecuteDispatchAsync(
                header,
                ct => activation.InvokeActorPacketAsync(
                    descriptor,
                    actor,
                    header,
                    body,
                    ct),
                CancellationToken.None)
            .ConfigureAwait(false);
    }

    private static ValueTask DispatchEntryActorPartsAsync(
        ZLinkFrameworkRuntime runtime,
        IReadOnlyList<ZLinkBackendActorPart> parts,
        CancellationToken cancellationToken)
    {
        var pipeline = new ZLinkActorInboundPipeline(
            runtime,
            new ZLinkEntrySpotActorInboundEndpoint(runtime));
        var frames = ZLinkActorHandoffIngress.CaptureMovingFrames(runtime, parts);
        return pipeline.DispatchAsync(frames, cancellationToken);
    }

    private static ZlinkStreamHeader CreateHeader(string name)
    {
        return new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            name,
            ZlinkStreamMetadata.Empty);
    }

    private static IReadOnlyList<ZLinkBackendActorPart>
        CreateStaleManagedParts(
            ZLinkBackendActorRef actor,
            ZLinkServiceWireCodec.RequestSourceFence source,
            MeshOperationId operation,
            IReadOnlyList<Message> messages,
            uint flags,
            Func<IReadOnlyList<Message>, SendFlags, SubmitResult>?
                directReply = null,
            byte messageFollowHopCount = 0)
    {
        var route = new ZLinkBackendActorRouteContext(
            operation,
            MessageFollowHopCount: messageFollowHopCount,
            TargetNodeGeneration: 43,
            AuthorityOwnerGeneration: 47,
            OwnerLeaseGeneration: 53,
            ReplyRequestId: flags == 0 ? 0 : operation.Low,
            ReplyFlags: flags,
            DeadlineUnixMs: 59);
        return messages.Select((message, index) =>
                new ZLinkBackendActorPart(
                    actor,
                    source.NodeRid,
                    default,
                    flags == 0 ? 0 : operation.Low,
                    flags,
                    message,
                    More: index + 1 < messages.Count,
                    RouteContext: route,
                    SourceNodeGeneration: source.NodeGeneration,
                    RequestSource: source,
                    DirectReply: index == 0 ? directReply : null))
            .ToArray();
    }

    private static ZLinkEntrySpotActivation CreateActivation(IServiceProvider services)
    {
        return CreateActivationWithRuntime(services, new CapturingSpot()).Activation;
    }

    private static async Task<(ZLinkFrameworkRuntime Runtime, ZLinkBackendActorRef ActorRef)> CreateStartedRuntimeAsync(
        CapturingSpotNode node,
        CapturingMessageFlowObserver? messageFlowObserver = null,
        ZLinkDiagnosticsLevel? messageFlowMode = null,
        IZLinkAutoConnectTopologyQuery? topology = null,
        bool includeJoinTarget = false,
        Type? entrySpotType = null,
        Type? userSpotType = null,
        BlockingSpotCreateProbe? blockingCreateProbe = null,
        BlockingActorJoinProbe? blockingActorJoinProbe = null,
        CommittedJoinRetryProbe? committedJoinRetryProbe = null,
        bool includeActorFactory = true,
        bool includeSpotRoute = false,
        bool preseedActorOwnership = true,
        TimeSpan? defaultRequestTimeout = null,
        ZLinkUserSpotExecutionMode userSpotExecutionMode =
            ZLinkUserSpotExecutionMode.SpotWide,
        bool includeInstanceSpotRoute = false,
        Func<IZLinkLocationStore, IZLinkLocationStore>?
            locationStoreWrapper = null,
        IZLinkSpotRetireTarget? retireTarget = null,
        IZLinkRelocationStore? relocationStore = null,
        DispatchProbe? dispatchProbe = null,
        bool includeEntryChannelMembership = false,
        bool includeImmediateIngressHandlers = false,
        bool includeEntrySpotActivation = true)
    {
        const string locationOwnerId = "entry-spot-dispatch-owner";
        var locationTime = new ManualTimeProvider();
        var locationProvider = new ZLinkInMemoryProviderLocationStore(locationTime);
        var runtimeLocationProvider =
            locationStoreWrapper?.Invoke(locationProvider) ?? locationProvider;
        var locationStore = new ZLinkProviderLocationRepository(locationProvider);
        var runtimeLocationStore = new ZLinkProviderLocationRepository(
            runtimeLocationProvider);
        await locationStore.ClaimLiveOwnerAsync(
            locationOwnerId,
            TimeSpan.FromMinutes(5));
        var locationOptions = new ZLinkLocationOptions
        {
            PollingInterval = TimeSpan.Zero
        };
        var locationResolvers = new ZLinkStoreLocationResolvers(
            runtimeLocationStore,
            new ZLinkOwnerLeaseTracker(
                runtimeLocationStore,
                locationOptions,
                locationTime),
            new ZLinkObservedLocationGenerations(),
            options: locationOptions);
        var locationRuntime = new ZLinkLocationRuntime(
            locationOptions,
            runtimeLocationStore,
            locationTime);
        Assert.True(await locationRuntime.RenewOwnerLeaseOnceAsync());
        var locationLifecycle = new ZLinkLocationLifecycle(
            locationRuntime,
            locationResolvers);
        var actorRef = node.ActorLookupResult ?? new ZLinkBackendActorRef(
            RoutingId.From(includeActorFactory ? "entry-node" : "actor-node"),
            "actor-a",
            1);
        var serviceCollection = new ServiceCollection()
            .AddSingleton<FlowJoinProbe>()
            .AddSingleton<LocalEntryJoinProbe>()
            .AddSingleton<IZLinkAutoConnectTopologyQuery>(
                topology ?? KnownRouteMeshTopology.Instance)
            .AddSingleton(locationRuntime)
            .AddSingleton(locationLifecycle)
            .AddSingleton(locationResolvers)
            .AddSingleton(new ZLinkLocationAddressResolvers(
                locationResolvers,
                new ZLinkSpotHandleRegistry()))
            .AddSingleton(blockingCreateProbe ?? new BlockingSpotCreateProbe())
            .AddSingleton(blockingActorJoinProbe ?? new BlockingActorJoinProbe())
            .AddSingleton(committedJoinRetryProbe ?? new CommittedJoinRetryProbe())
            .AddSingleton<MeshRouteContextCapture>()
            .AddTransient<ProbeActorFactory>()
            .AddTransient<ProbeActorRequestHandler>()
            .AddTransient<ProbeActorFlowJoinRequestHandler>()
            .AddTransient<ProbeActorDestroyRequestHandler>()
            .AddTransient<ProbeActorThrowingRequestHandler>()
            .AddTransient<MeshChannelRequestHandler>()
            .AddTransient<MeshRouteRequestHandler>();
        if (dispatchProbe is not null)
        {
            serviceCollection.AddSingleton(dispatchProbe);
            serviceCollection.AddTransient<ProbeActorSendHandler>();
        }
        if (retireTarget is not null)
            serviceCollection.AddSingleton<IZLinkSpotRetireTarget>(retireTarget);
        var services = serviceCollection.BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration
        {
            DefaultRequestTimeout =
                defaultRequestTimeout ?? TimeSpan.FromSeconds(1),
            ImplicitHandlerAutoRegistrationEnabled = false
        };
        registration.Locations.StoreInstance = runtimeLocationProvider;
        if (relocationStore is not null)
            registration.Locations.RelocationStoreInstance = relocationStore;
        if (messageFlowMode is { } mode)
            registration.DispatchOptions.Diagnostics.SetLevel(mode);
        else if (messageFlowObserver is not null)
            registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Detailed);
        registration.SpotNodes["entry"] = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "entry",
            RoutingId = RoutingId.From("entry-node"),
            Router = new ZLinkSpotRouterCapabilityRegistration { BindEndpoint = "inproc://entry" },
            EntrySpotType = includeEntrySpotActivation
                ? entrySpotType ?? typeof(ProbeEntrySpot)
                : null,
        };
        registration.SpotNodes["entry"].Router!.AcquisitionMode =
            ZLinkPeerAcquisitionMode.AutoConnect;
        if (includeImmediateIngressHandlers)
        {
            registration.SpotNodes["entry"].RouteRequestHandlers.Add(
                new ZLinkRouteHandlerRegistration(
                    typeof(MeshRouteRequestHandler),
                    typeof(MeshRequest),
                    typeof(MeshReply),
                    "ExactRequest"));
            var startupChannel = new ZLinkMeshChannelMembership
            {
                ChannelName = "startup-channel"
            };
            startupChannel.RequestHandlers.Add(
                new ZLinkChannelHandlerRegistration(
                    typeof(MeshChannelRequestHandler),
                    typeof(MeshRequest),
                    typeof(MeshReply),
                    "ExactRequest"));
            registration.SpotNodes["entry"].ChannelMemberships.Add(startupChannel);
        }
        if (includeEntryChannelMembership)
        {
            registration.SpotNodes["entry"].ChannelMemberships.Add(
                new ZLinkMeshChannelMembership { ChannelName = "entry" });
        }
        if (includeActorFactory)
        {
            registration.SpotNodes["entry"].ActorFactories["probe"] = typeof(ProbeActorFactory);
            registration.SpotNodes["entry"].ActorRelocations["probe"] =
                new ZLinkObjectRelocationRegistration(
                    typeof(ProbeActor),
                    new ZLinkObjectPlacementOptions(),
                    PolicyKind: 0,
                    AdapterType: null,
                    AdapterInvoker: null);
        }
        if (includeJoinTarget)
        {
            registration.SpotNodes["entry"].SpotFactories.Add(typeof(JoinTargetSpot));
            registration.SpotNodes["entry"].SpotRelocations[typeof(JoinTargetSpot).FullName!] =
                new ZLinkObjectRelocationRegistration(
                    typeof(JoinTargetSpot),
                    new ZLinkObjectPlacementOptions(),
                    PolicyKind: 0,
                    AdapterType: null,
                    AdapterInvoker: null);
        }
        if (userSpotType is not null)
        {
            registration.SpotNodes["entry"].SpotFactories.Add(userSpotType);
            registration.SpotNodes["entry"].UserSpotFactoryOptions[userSpotType] =
                new ZLinkUserSpotFactoryConfiguration(
                    ExecutionMode: userSpotExecutionMode);
            registration.SpotNodes["entry"].SpotRelocations[userSpotType.FullName!] =
                new ZLinkObjectRelocationRegistration(
                    userSpotType,
                    new ZLinkObjectPlacementOptions(),
                    PolicyKind: 0,
                    AdapterType: null,
                    AdapterInvoker: null);
        }
        if (includeActorFactory || includeJoinTarget || userSpotType is not null)
        {
            registration.SpotNodes["entry"].ObjectRole = ZLinkMeshNodeObjectRole.Server;
            registration.SpotNodes["entry"].ObjectRoleSelected = true;
        }
        if (includeInstanceSpotRoute)
        {
            registration.SpotNodes["entry"].InstanceSpotFactories[
                    "Tests.InstanceSpot"] =
                new ZLinkInstanceSpotFactoryRegistration(
                    typeof(ProbeInstanceSpot),
                    new ZLinkInstanceSpotFactoryConfiguration());
            registration.SpotNodes["entry"].InstanceSpotRelocations[
                    "Tests.InstanceSpot"] =
                new ZLinkObjectRelocationRegistration(
                    typeof(ProbeInstanceSpot),
                    new ZLinkObjectPlacementOptions(),
                    PolicyKind: 0,
                    AdapterType: null,
                    AdapterInvoker: null);
            registration.SpotNodes["entry"].ObjectRole = ZLinkMeshNodeObjectRole.Server;
            registration.SpotNodes["entry"].ObjectRoleSelected = true;
        }
        registration.ActorCatalog.Build(registration.SpotNodes.Values);
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new CapturingBackendAdapterFactory(node),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(services.GetRequiredService<IServiceScopeFactory>(), registration));
        node.NodeDisposeHandler = async () =>
        {
            _ = await locationLifecycle.RemoveMeshNodeDescriptorAsync(
                    new ZLinkMeshNodeDescriptorKey("entry", RoutingId.From("entry-node")))
                .ConfigureAwait(false);
        };
        await runtime.StartAsync(CancellationToken.None);
        var preparingDescriptor = (await locationStore.ListMeshNodesAsync("entry", default))
            .Items
            .SingleOrDefault(item => item.Rid == RoutingId.From("entry-node"));
        if (preparingDescriptor is not null)
        {
            var servingDescriptor = preparingDescriptor with
            {
                DescriptorRevision = checked(preparingDescriptor.DescriptorRevision + 1),
                State = ZLinkFrameworkRuntimeState.Serving
            };
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                (await locationRuntime.WriteDescriptorAsync(
                    servingDescriptor,
                    ZLinkLocationWriteIntent.Renew)).Status);
        }
        registration.SpotNodes["entry"].SpotRelocations.Clear();
        var localSpotType = userSpotType ?? (includeJoinTarget ? typeof(JoinTargetSpot) : null);
        if (localSpotType is not null)
        {
            node.BeforeGetOrCreateSpot = spotId =>
                PublishLocalSpotAuthorityAsync(
                        locationStore,
                        locationRuntime.OwnerToken,
                        localSpotType.FullName!,
                        spotId)
                    .AsTask()
                    .GetAwaiter()
                    .GetResult();
        }
        if (includeActorFactory && preseedActorOwnership)
        {
            await CreateTrackedLocalActorOwnershipAsync(
                locationStore,
                locationRuntime.OwnerToken,
                locationLifecycle.ActorOwnership,
                actorRef,
                CancellationToken.None);
        }
        else
        {
            Assert.NotNull(await AuthorityLocationTestFixture.PublishActorAsync(
                locationStore,
                InMemoryLocationStoreTests.Actor(locationOwnerId, actorRef.ActorId) with
                {
                    MeshName = "entry",
                    ActorRef = new ActorRef(
                        actorRef.ActorId,
                        actorRef.Generation,
                        "entry",
                        actorRef.NodeRid),
                    OwnerNodeRid = actorRef.NodeRid,
                    OwnerNodeGeneration = 1
                }));
        }
        if (includeSpotRoute)
        {
            Assert.NotNull(await AuthorityLocationTestFixture.PublishSpotAsync(
                locationStore,
                InMemoryLocationStoreTests.Spot(locationOwnerId, "spot-ready") with
                {
                    MeshName = "entry",
                    OwnerNodeRid = RoutingId.From("spot-node"),
                    OwnerNodeGeneration = 1
                }));
        }
        if (includeInstanceSpotRoute)
        {
            _ = await PublishLocalInstanceSpotAuthorityAsync(
                locationStore,
                locationRuntime.OwnerToken,
                "Tests.InstanceSpot",
                "spot-ready");
        }
        return (runtime, actorRef);
    }

    private static ZLinkBackendRouteReceived CreateImmediateMeshRequest(
        string value,
        string? channelName,
        ulong requestSequence,
        TaskCompletionSource<string> reply)
    {
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Request,
            channelName ?? "entry",
            "ExactRequest",
            ZLinkEnvelopeCodec.DefaultContentType,
            $"startup-{requestSequence}",
            null,
            null,
            null,
            null);
        var parts = ZLinkEnvelopeCodec.EncodeParts(
            header,
            new MeshRequest(value),
            typeof(MeshRequest),
            null);
        return new ZLinkBackendRouteReceived(
            parts,
            RoutingId.From("startup-source"),
            spotId: null,
            requestSequence,
            reply: (replyParts, _) =>
            {
                var decoded = ZLinkEnvelopeCodec.DecodeBody(
                    replyParts,
                    typeof(MeshReply));
                reply.TrySetResult(Assert.IsType<MeshReply>(decoded).Value);
                return SubmitResult.Ok;
            },
            channelName);
    }

    private static async ValueTask<ulong> PublishLocalSpotAuthorityAsync(
        IZLinkLocationRepository store,
        ZLinkLocationOwnerToken owner,
        string stableType,
        string spotId)
    {
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId);
        if (await store.ReadAuthorityAsync(key) is ZLinkAuthorityReadResult.Found found)
            return found.Snapshot.ObjectGeneration;

        var payload = ZLinkUserSpotAuthorityPayloadCodec.Encode(
            new ZLinkUserSpotAuthorityPayload(
                ZLinkUserSpotAuthorityState.Ready,
                stableType,
                spotId,
                owner.OwnerId,
                checked((ulong)owner.LeaseGeneration),
                "entry",
                RoutingId.From("entry-node"),
                1));
        await ReserveAndCommitLocalAuthorityAsync(
            store,
            key,
            ZLinkPlacementObjectKind.UserSpot,
            stableType,
            RoutingId.From("entry-node"),
            owner,
            payload,
            new ZLinkCapacityVector(
                0,
                1,
                new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.UserSpot,
                    stableType,
                    1)));
        var committed = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(key));
        return committed.Snapshot.ObjectGeneration;
    }

    private static async ValueTask<ulong> PublishLocalInstanceSpotAuthorityAsync(
        IZLinkLocationRepository store,
        ZLinkLocationOwnerToken owner,
        string stableType,
        string spotId)
    {
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spotId);
        if (await store.ReadAuthorityAsync(key) is ZLinkAuthorityReadResult.Found found)
            return found.Snapshot.ObjectGeneration;

        var payload = ZLinkInstanceSpotAuthorityPayloadCodec.Encode(
            new ZLinkInstanceSpotAuthorityPayload(
                ZLinkInstanceSpotAuthorityState.Ready,
                spotId,
                stableType,
                "entry",
                RoutingId.From("entry-node"),
                1,
                owner.OwnerId,
                checked((ulong)owner.LeaseGeneration),
                RecoveryReference: null,
                RecoveryChecksum: 0,
                ReplayCursor: 0));
        await ReserveAndCommitLocalAuthorityAsync(
            store,
            key,
            ZLinkPlacementObjectKind.InstanceSpot,
            stableType,
            RoutingId.From("entry-node"),
            owner,
            payload,
            new ZLinkCapacityVector(
                0,
                1,
                new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.InstanceSpot,
                    stableType,
                    1)));
        var committed = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(key));
        return committed.Snapshot.ObjectGeneration;
    }

    private sealed class KnownRouteMeshTopology : IZLinkAutoConnectTopologyQuery
    {
        public static KnownRouteMeshTopology Instance { get; } = new();

        public ZLinkRouteMeshTargetClassification ClassifyRouteMeshTarget(
            string meshName,
            RoutingId nodeRid)
        {
            _ = meshName;
            _ = nodeRid;
            return ZLinkRouteMeshTargetClassification.ReadyEligible;
        }

    }

    private sealed class TestRouteMeshTopology(
        ZLinkRouteMeshTargetClassification classification,
        IReadOnlyList<ZLinkRouteMeshPeerIdentity>? CompleteSnapshot)
        : IZLinkAutoConnectTopologyQuery
    {
        public ZLinkRouteMeshTargetClassification ClassifyRouteMeshTarget(
            string meshName,
            RoutingId nodeRid)
        {
            _ = meshName;
            _ = nodeRid;
            return classification;
        }

        public IReadOnlyList<ZLinkRouteMeshPeerIdentity>? GetCompleteRouteMeshPeers(
            string meshName)
        {
            _ = meshName;
            return CompleteSnapshot;
        }
    }

    private static void ConfigureNotConnectedEntryJoin(CapturingSpotNode node)
    {
        node.EntrySpotJoinResult = new ZLinkBackendActorJoinEntrySpotResult(
            RequestResult.NotConnected,
            0,
            new ZLinkBackendActorRef(RoutingId.From("actor-node"), "actor-a", 1),
            RoutingId.From("entry-node"),
            "entry-spot",
            0,
            0);
    }

    private static (ZLinkEntrySpotActivation Activation, ZLinkFrameworkRuntime Runtime) CreateActivationWithRuntime(
        IServiceProvider services,
        IZLinkBackendSpot spot,
        Type? entrySpotType = null)
    {
        var registration = new ZLinkFrameworkRegistration();
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new ThrowingBackendAdapterFactory(),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(services.GetRequiredService<IServiceScopeFactory>(), registration));

        var scope = services.CreateAsyncScope();
        var activation = new ZLinkEntrySpotActivation(
            runtime,
            services,
            scope,
            spot,
            "entry-id",
            entrySpotType ?? typeof(ProbeEntrySpot),
            RoutingId.From("entry-node"),
            "entry",
            "entry-channel",
            TimeSpan.FromSeconds(5),
            new ZLinkSpotOutboundTransport(
                spot,
                TimeSpan.FromSeconds(1),
                CancellationToken.None));
        activation.InitializeRuntimeResources();
        return (activation, runtime);
    }

    private static ProbeActor RegisterProbeActor(
        ZLinkFrameworkRuntime runtime,
        ZLinkBackendActorRef actorRef)
    {
        var state = runtime.GetOrCreateActorState(actorRef.ActorId);
        state.BindNativeActorRef(actorRef);
        var operation = state.GetOrStartActorCreationAsync(
                "probe",
                failIfExists: false,
                () =>
                {
                    var context = state.GetOrCreateContext(() => new ZLinkActorContext(
                        runtime,
                        state,
                        "entry",
                        actorRef.Generation,
                        spotId: null,
                        new ZLinkBoundSessionService(runtime)));
                    return Task.FromResult<IZLinkActor>(
                        new ProbeActor(actorRef.ActorId, context));
                },
                CancellationToken.None)
            .AsTask()
            .GetAwaiter()
            .GetResult();
        var actor = Assert.IsType<ProbeActor>(operation.Task.GetAwaiter().GetResult());
        state.BindActorInstance(actor);
        return actor;
    }

    private static IReadOnlyList<ZLinkBackendActorPart> CreateActorRequestParts(
        ZLinkBackendActorRef actorRef,
        string packetName,
        string value,
        ulong requestId,
        uint flags,
        bool malformedPayload = false,
        string sourceNodeRid = "entry-node",
        string? sourceSessionRid = "source-session",
        ZlinkStreamMessageKind kind = ZlinkStreamMessageKind.Request)
    {
        var sourceNode = RoutingId.From(sourceNodeRid);
        var sourceSession = sourceSessionRid is null
            ? default
            : RoutingId.From(sourceSessionRid);
        var header = new ZlinkStreamHeader(
            kind,
            ZlinkStreamCodec.Json,
            kind == ZlinkStreamMessageKind.Request
                ? ZlinkStreamHeaderFlags.HasRequestSeq
                : ZlinkStreamHeaderFlags.None,
            kind == ZlinkStreamMessageKind.Request
                ? new ZlinkStreamRequestSeq(7)
                : null,
            packetName,
            ZlinkStreamMetadata.Empty,
            "corr-1");
        var replyRoute = requestId != 0 && (flags & 1) != 0
            ? new ZLinkBackendActorRouteContext(
                default,
                0,
                1,
                1,
                1,
                requestId,
                flags,
                "test-reply-capability")
            : default;
        return
        [
            new ZLinkBackendActorPart(
                actorRef,
                // The runtime's own node rid: these parts model a locally
                // relayed session frame (a differing rid now means a remote
                // session node and takes the relay plane instead).
                sourceNode,
                sourceSession,
                requestId,
                flags,
                Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(header).Span),
                true,
                RouteContext: replyRoute),
            new ZLinkBackendActorPart(
                actorRef,
                sourceNode,
                sourceSession,
                requestId,
                flags,
                malformedPayload
                    ? Message.From("{")
                    : Message.From(ZLinkEnvelopeCodec.EncodeJsonBytes(value, typeof(string))),
                false,
                RouteContext: replyRoute)
        ];
    }

    private static bool IsDisposed(Message message)
    {
        try
        {
            _ = message.Size;
            return false;
        }
        catch (ObjectDisposedException)
        {
            return true;
        }
    }

    private static (ZlinkStreamHeader Header, T Payload) DecodeReplyFrame<T>(byte[] frame)
    {
        var headerSize = BinaryPrimitives.ReadUInt16BigEndian(frame.AsSpan(0, 2));
        var payloadSize = BinaryPrimitives.ReadUInt32BigEndian(frame.AsSpan(2, 4));
        var header = ZLinkStreamProtocolDefaults.DecodeHeader(frame.AsMemory(6, headerSize));
        var payload = JsonSerializer.Deserialize<T>(
                          frame.AsSpan(6 + headerSize, checked((int)payloadSize)),
                          ZLinkJsonSerializerOptions.Default)
                      ?? throw new InvalidOperationException("Reply payload was null.");
        return (header, payload);
    }

    private static ZlinkStreamHeader DecodeFrameHeader(byte[] frame)
    {
        var headerSize = BinaryPrimitives.ReadUInt16BigEndian(frame.AsSpan(0, 2));
        return ZLinkStreamProtocolDefaults.DecodeHeader(frame.AsMemory(6, headerSize));
    }

    // RouteMesh 10.0.0 delivers routed traffic to the entry spot dispatch pump as a
    // framework-owned ZLinkBackendRouteReceived (the node pump drains Core claims
    // into these records). The record owns the encoded parts and is disposed by the
    // dispatch path, so we mint fresh parts per call and hand them straight over.
    private static ZLinkBackendRouteReceived CreateRoutedReceived(string value)
    {
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Command,
            "entry-channel",
            nameof(ProbeRouteMessage));
        var parts = ZLinkClientCallCodec.EncodeEnvelopeParts(
            header,
            new ProbeRouteMessage(value),
            null);
        return new ZLinkBackendRouteReceived(
            parts,
            sourceNodeRid: RoutingId.From("route-source-node"),
            spotId: "route-receiver-spot",
            requestSeq: null,
            reply: null);
    }

    private sealed class DispatchProbe
    {
        public ConcurrentQueue<string> Events { get; } = new();

        public TaskCompletionSource ActorAFirstStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource ActorASecondStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource ActorBStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource ReleaseActorAFirst { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    internal sealed class ProbeActor(string actorId, IZLinkActorContext? context = null) : IZLinkActor
    {
        // Test-only convenience identity for dispatch fixtures that do not create a runtime Context.
        public string ActorId { get; } = actorId;

        public IZLinkActorContext Context { get; } = context ?? new TestActorContext(actorId);
    }

    private sealed class ProbeActorFactory : IZLinkActorFactory
    {
        public ValueTask<IZLinkActor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult<IZLinkActor>(new ProbeActor(context.ActorId, context));
        }
    }

    private sealed class CreationProbeActor(IZLinkActorContext context) : IZLinkActor
    {
        public IZLinkActorContext Context { get; } = context;

        public void Configure()
        {
        }
    }

    private sealed class CreationProbeActorFactory : IZLinkActorFactory
    {
        public ValueTask<IZLinkActor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Assert.False(string.IsNullOrWhiteSpace(context.ActorId));
            Assert.False(string.IsNullOrWhiteSpace(context.MeshName));
            Assert.NotEqual(0UL, context.ObjectGeneration);
            return ValueTask.FromResult<IZLinkActor>(new CreationProbeActor(context));
        }
    }

    private sealed class ControlledCreationProbe
    {
        public TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class ControlledCreationProbeActorFactory(ControlledCreationProbe probe)
        : IZLinkActorFactory
    {
        public async ValueTask<IZLinkActor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken cancellationToken = default)
        {
            probe.Started.TrySetResult();
            await probe.Release.Task.WaitAsync(cancellationToken);
            return new CreationProbeActor(context);
        }
    }

    private sealed class FailingPublishActorLifecycle : IZLinkActorLocationLifecycle
    {
        public int ReleaseCalls { get; private set; }

        public Action? BeforeRelease { get; set; }

        public async ValueTask<ZLinkActorClaimActivation<TActor>> ExecuteActorClaimThenActivateAsync<TActor>(
            ZLinkMeshName meshName,
            string actorType,
            ZLinkActorId actorId,
            RoutingId nodeRid,
            Func<CancellationToken, ValueTask>? deactivate,
            Func<CancellationToken, ValueTask<TActor>> activate,
            CancellationToken cancellationToken,
            ZLinkActorClaimMode claimMode = ZLinkActorClaimMode.NewOwner)
            where TActor : class
        {
            _ = meshName;
            _ = actorType;
            _ = actorId;
            _ = nodeRid;
            _ = deactivate;
            _ = claimMode;
            return new ZLinkActorClaimActivation<TActor>(
                await activate(cancellationToken),
                null);
        }

        public ValueTask PublishActorRefAsync(
            ZLinkActorId actorId,
            ActorRef actorRef,
            CancellationToken cancellationToken = default)
            => ValueTask.FromException(new InvalidOperationException("publish failed"));

        public ValueTask ReleaseActorAsync(
            ZLinkActorId actorId,
            CancellationToken cancellationToken = default)
        {
            BeforeRelease?.Invoke();
            ReleaseCalls++;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class FailOnceRemoveActorStore(IZLinkLocationRepository inner)
        : global::Zlink.Framework.UnitTests.ZLinkLocationStoreTestDouble
    {
        private int _removeAttempts;

        public override ValueTask<long> RemoveAllByOwnerAsync(
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default) =>
            inner.RemoveAllByOwnerAsync(owner, cancellationToken);

        public override ValueTask<ZLinkLocationWriteResult> UpdateMeshNodeAsync(
            ZLinkMeshNodeDescriptor descriptor,
            ZLinkLocationWriteIntent intent,
            CancellationToken cancellationToken = default) =>
            inner.UpdateMeshNodeAsync(descriptor, intent, cancellationToken);

        public override ValueTask<ZLinkLocationWriteStatus> RemoveMeshNodeAsync(
            ZLinkMeshNodeDescriptorKey key,
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default) =>
            inner.RemoveMeshNodeAsync(key, owner, cancellationToken);

        public override ValueTask<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> ListMeshNodesAsync(
            string meshName,
            ZLinkPageRequest page,
            CancellationToken cancellationToken = default) =>
            inner.ListMeshNodesAsync(meshName, page, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default) =>
            inner.ClaimOwnerLeaseAsync(ownerId, leaseTtl, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default) =>
            inner.ReadOwnerLeaseAsync(ownerId, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default) =>
            inner.RenewOwnerLeaseAsync(token, leaseTtl, cancellationToken);

        public override ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default) =>
            inner.ReleaseOwnerLeaseAsync(token, cancellationToken);

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default) =>
            inner.ReadAuthorityAsync(key, cancellationToken);

        public override ValueTask<ZLinkAuthorityCompareExchangeResult>
            CompareExchangeAuthorityAsync(
                ZLinkAuthorityKey key,
                string expectedStoreVersion,
                ZLinkAuthorityMutation mutation,
                CancellationToken cancellationToken = default)
        {
            if (mutation is ZLinkAuthorityMutation.Delete
                && Interlocked.Increment(ref _removeAttempts) == 1)
                return ValueTask.FromException<ZLinkAuthorityCompareExchangeResult>(
                    new InvalidOperationException("release failed"));

            return inner.CompareExchangeAuthorityAsync(
                key, expectedStoreVersion, mutation, cancellationToken);
        }

        public override ValueTask<ZLinkAuthorityScanResult> ListAuthoritiesAsync(
            string prefix,
            ZLinkAuthorityScanCursor? cursor,
            int limit,
            CancellationToken cancellationToken = default) =>
            inner.ListAuthoritiesAsync(prefix, cursor, limit, cancellationToken);

        public override ValueTask<ZLinkObjectReserveResult> ReserveAsync(
            ZLinkObjectReservationRequest request,
            CancellationToken cancellationToken = default) =>
            inner.ReserveAsync(request, cancellationToken);

        public override ValueTask<ZLinkObjectCommitResult> CommitAsync(
            ZLinkObjectReservation reservation,
            ReadOnlyMemory<byte> readyPayload,
            CancellationToken cancellationToken = default) =>
            inner.CommitAsync(reservation, readyPayload, cancellationToken);

        public override ValueTask<ZLinkObjectCreationCompleteResult> CompleteCreationAsync(
            ZLinkObjectReservation reservation,
            ZLinkObjectCreationCompletion completion,
            CancellationToken cancellationToken = default) =>
            inner.CompleteCreationAsync(reservation, completion, cancellationToken);

        public override ValueTask<ZLinkCreationTerminalReadResult> ReadCreationTerminalAsync(
            ZLinkCreationOperationId operation,
            CancellationToken cancellationToken = default) =>
            inner.ReadCreationTerminalAsync(operation, cancellationToken);

        public override ValueTask<ZLinkObjectAbortResult> AbortAsync(
            ZLinkObjectReservation reservation,
            CancellationToken cancellationToken = default) =>
            inner.AbortAsync(reservation, cancellationToken);

        public override ValueTask<ZLinkAggregatePrepareResult> PrepareAggregateAsync(
            ZLinkAggregatePrepareRequest request,
            CancellationToken cancellationToken = default) =>
            inner.PrepareAggregateAsync(request, cancellationToken);

        public override ValueTask<ZLinkAggregateCommitResult> CommitAggregateAsync(
            ZLinkAggregateFence fence,
            CancellationToken cancellationToken = default) =>
            inner.CommitAggregateAsync(fence, cancellationToken);

        public override ValueTask<ZLinkAggregateAbortResult> AbortAggregateAsync(
            ZLinkAggregateFence fence,
            CancellationToken cancellationToken = default) =>
            inner.AbortAggregateAsync(fence, cancellationToken);
    }

    private sealed class ProbeEntrySpot(IZLinkEntrySpotContext context) : IZLinkEntrySpot
    {
        public IZLinkEntrySpotContext Context { get; } = context;

        public void Configure()
        {
            Context.Handlers.AddPacket<ProbeRouteHandler>();
            Context.Handlers.AddHandler<ProbeActorSendHandler>("first");
            Context.Handlers.AddHandler<ProbeActorSendHandler>("second");
            Context.Handlers.AddHandler<ProbeActorRequestHandler>("request");
            Context.Handlers.AddHandler<ProbeActorFlowJoinRequestHandler>("flow-join");
            Context.Handlers.AddHandler<ProbeActorDestroyRequestHandler>("destroy-request");
            Context.Handlers.AddHandler<ProbeActorThrowingRequestHandler>("throw");
        }
    }

    private sealed class ProbeInstanceSpot(IZLinkInstanceSpotContext context)
        : IZLinkInstanceSpot
    {
        public IZLinkInstanceSpotContext Context { get; } = context;
    }

    private sealed class EmptyUserSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
    }

    private enum SchedulerProbePublishMode
    {
        Commit,
        DurablyAborted,
        Unknown
    }

    private sealed class SchedulerProbeTarget(
        Func<IZLinkLocationRepository> authorityStore,
        IZLinkRelocationRepository relocationStore,
        SchedulerProbePublishMode publishMode) : IZLinkSpotRetireTarget
    {
        private ZLinkPreparedSpotRetireStaging? _staging;

        internal int StageCalls { get; private set; }
        internal int PublishCalls { get; private set; }
        internal int AbortCalls { get; private set; }

        public ValueTask<ZLinkSpotRetireReservation?> TryReserveAsync(
            ZLinkSpotRetireInventory inventory,
            ZLinkRelocationTargetSelection selection,
            CancellationToken cancellationToken)
        {
            _ = selection;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult<ZLinkSpotRetireReservation?>(
                new ZLinkSpotRetireReservation(
                    inventory,
                    new ZLinkMeshNodeDescriptorKey(
                        inventory.MeshName,
                        inventory.SourceNodeRid),
                    inventory.SourceNodeLifecycleGeneration,
                    new ZLinkCapacityVector(
                        0,
                        1,
                        new ZLinkSpotTypeCapacityDelta(
                            ZLinkPlacementObjectKind.UserSpot,
                            inventory.StableType,
                            1)),
                    inventory.SourceOwner));
        }

        public ValueTask StageAsync(
            ZLinkSpotRetireReservation reservation,
            ZLinkPreparedSpotRetireStaging relocation,
            CancellationToken cancellationToken)
        {
            _ = reservation;
            _staging = relocation;
            StageCalls++;
            // The command boundary is already published by the scheduler. The
            // missing response must therefore enter reconciliation, even when
            // the transport reports cancellation.
            return ValueTask.FromException(
                new OperationCanceledException(
                    "The Stage acknowledgement was not observed.",
                    cancellationToken));
        }

        public async ValueTask<ulong> PublishAsync(
            ZLinkSpotRetireReservation reservation,
            ZLinkAggregateRelocationPublished relocation,
            CancellationToken cancellationToken)
        {
            PublishCalls++;
            cancellationToken.ThrowIfCancellationRequested();
            switch (publishMode)
            {
                case SchedulerProbePublishMode.DurablyAborted:
                    throw new ZLinkCanonicalRelocationDurablyAbortedException(
                        "The target durably aborted the aggregate.");
                case SchedulerProbePublishMode.Unknown:
                    throw new InvalidOperationException(
                        "The target authority result is unknown.");
                case SchedulerProbePublishMode.Commit:
                    break;
                default:
                    throw new InvalidOperationException(
                        "The scheduler probe publish mode is invalid.");
            }

            var staging = _staging
                ?? throw new InvalidOperationException(
                    "The scheduler probe has no staged aggregate.");
            var request = new ZLinkAggregateRelocationRequest(
                relocation.Envelope.AggregateId,
                relocation.Envelope.AggregateGeneration,
                1,
                staging.Participants,
                reservation.TargetDescriptor,
                reservation.TargetDescriptorLifecycleGeneration,
                reservation.Capacity,
                reservation.TargetOwner,
                relocation.Envelope);
            var store = authorityStore();
            var coordinator = new ZLinkAggregateRelocationCoordinator(
                store,
                relocationStore);
            _ = await coordinator.PublishAsync(
                    request,
                    CancellationToken.None,
                    relocation.Relocation)
                .ConfigureAwait(false);
            var spotKey = relocation.Envelope.Participants.Single(
                    static participant => participant.ObjectKind
                        is ZLinkPlacementObjectKind.UserSpot
                        or ZLinkPlacementObjectKind.InstanceSpot)
                .AuthorityKey;
            var read = await store.ReadAuthorityAsync(
                    spotKey,
                    CancellationToken.None)
                .ConfigureAwait(false);
            return read is ZLinkAuthorityReadResult.Found found
                ? found.Snapshot.AuthorityOwnerGeneration
                : throw new InvalidOperationException(
                    "The scheduler probe published no SPOT authority.");
        }

        public ValueTask AbortAsync(
            ZLinkSpotRetireReservation reservation,
            ZLinkAggregateFence? fence)
        {
            _ = reservation;
            _ = fence;
            AbortCalls++;
            return ValueTask.CompletedTask;
        }

        public ValueTask RelayCommittedAsync(
            ZLinkSpotRetireReservation reservation,
            ZLinkAggregateRelocationPublished relocation,
            IReadOnlyList<ZLinkAcceptedWorkRecord> held,
            CancellationToken cancellationToken)
        {
            _ = reservation;
            _ = relocation;
            _ = held;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.CompletedTask;
        }
    }

    private sealed class SchedulerProbeRelocationStore
        : IZLinkRelocationStore
    {
        private readonly Dictionary<string, byte[]> _payloads =
            new(StringComparer.Ordinal);

        public ValueTask<ZLinkBlobPutResult> PutAsync(
            ZLinkBlobReference reference,
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var bytes = payload.ToArray();
            var now = DateTimeOffset.UtcNow;
            var expiresAt = now + retention;
            if (_payloads.TryGetValue(reference.Value, out var current)
                && !current.AsSpan().SequenceEqual(bytes))
                return ValueTask.FromResult<ZLinkBlobPutResult>(
                    new ZLinkBlobPutResult.Conflict(now));
            var existed = _payloads.ContainsKey(reference.Value);
            _payloads[reference.Value] = bytes;
            return ValueTask.FromResult<ZLinkBlobPutResult>(
                existed
                    ? new ZLinkBlobPutResult.AlreadyStored(expiresAt, now)
                    : new ZLinkBlobPutResult.Stored(expiresAt, now));
        }

        public ValueTask<ZLinkBlobReadResult> ReadAsync(
            ZLinkBlobReference reference,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult<ZLinkBlobReadResult>(
                _payloads.TryGetValue(reference.Value, out var payload)
                    ? new ZLinkBlobReadResult.Found(
                        payload,
                        now + TimeSpan.FromHours(24),
                        now)
                    : new ZLinkBlobReadResult.Missing(now));
        }

        public ValueTask<ZLinkBlobRenewResult> RenewAsync(
            ZLinkBlobReference reference,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult<ZLinkBlobRenewResult>(
                _payloads.ContainsKey(reference.Value)
                    ? new ZLinkBlobRenewResult.Renewed(
                        now + retention,
                        now)
                    : new ZLinkBlobRenewResult.Missing(now));
        }

        public ValueTask DeleteAsync(
            ZLinkBlobReference reference,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            _payloads.Remove(reference.Value);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class PerActorClosingProbeSpot(
        IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;

        internal TaskCompletionSource<ZLinkSpotCloseReason> ClosingInvoked
        {
            get;
        } = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ValueTask OnClosingAsync(
            ZLinkSpotClosingContext closing,
            CancellationToken cleanupCancellationToken)
        {
            cleanupCancellationToken.ThrowIfCancellationRequested();
            ClosingInvoked.TrySetResult(closing.Reason);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class SourceLeaveProbeSpot(
        IZLinkSpotContext context) : IZLinkSpot<ProbeActor>
    {
        public IZLinkSpotContext Context { get; } = context;

        internal int LeaveCount { get; private set; }

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken) =>
            ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());

        public ValueTask OnJoinedActorAsync(
            ProbeActor actor,
            CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(
            ProbeActor actor,
            CancellationToken cancellationToken)
        {
            LeaveCount++;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class ClosingTeardownOrderProbe
    {
        internal bool ClosingInvoked { get; set; }
        internal bool ScopeDisposed { get; set; }
        internal bool ClosingPrecededScopeDisposal { get; set; }
    }

    private sealed class ClosingTeardownDependency(
        ClosingTeardownOrderProbe order) : IAsyncDisposable
    {
        public ValueTask DisposeAsync()
        {
            order.ScopeDisposed = true;
            order.ClosingPrecededScopeDisposal = order.ClosingInvoked;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class ClosingTeardownProbeSpot(
        IZLinkSpotContext context,
        ClosingTeardownOrderProbe order) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;

        public ValueTask OnClosingAsync(
            ZLinkSpotClosingContext closing,
            CancellationToken cleanupCancellationToken)
        {
            cleanupCancellationToken.ThrowIfCancellationRequested();
            Assert.Equal(ZLinkSpotCloseReason.RelocationOut, closing.Reason);
            order.ClosingInvoked = true;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class RelocationReadyProbeSpot(
        ZLinkUserSpotActivation activation,
        bool failFirstRelocatedCallback = false) : IZLinkSpot
    {
        private int _relocatedCallbackCount;

        public IZLinkSpotContext Context { get; } = activation;

        internal TaskCompletionSource<ZLinkSpotRelocationReadyCompletion>
            NextCompletion { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal int RelocatedCallbackCount =>
            Volatile.Read(ref _relocatedCallbackCount);

        internal List<bool> AdmissionWasClosed { get; } = [];

        public ValueTask OnRelocationReadyCompletedAsync(
            ZLinkSpotRelocationReadyCompletion completion,
            CancellationToken cancellationToken)
        {
            _ = cancellationToken;
            AdmissionWasClosed.Add(!activation.IsRelocationReady);
            if (completion.Outcome
                == ZLinkSpotRelocationReadyOutcome.Relocated)
            {
                var count = Interlocked.Increment(
                    ref _relocatedCallbackCount);
                if (failFirstRelocatedCallback && count == 1)
                    throw new InvalidOperationException(
                        "relocation-ready callback retry probe");
            }

            NextCompletion.TrySetResult(completion);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class BlockingCreateSpot(
        IZLinkSpotContext context,
        BlockingSpotCreateProbe probe) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;

        public async ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            _ = request;
            _ = cancellationToken;
            probe.Started.TrySetResult();
            await probe.Release.Task.ConfigureAwait(false);
            return ZLinkSpotCreateResponse.Accept();
        }
    }

    private sealed class BlockingSpotCreateProbe
    {
        public TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class BlockingActorJoinSpot(
        IZLinkSpotContext context,
        BlockingActorJoinProbe probe) : IZLinkSpot<ProbeActor>
    {
        public IZLinkSpotContext Context { get; } = context;

        public async ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            _ = actorId;
            _ = request;
            _ = cancellationToken;
            probe.Started.TrySetResult();
            await probe.Release.Task.ConfigureAwait(false);
            return ZLinkSpotActorJoinResult.Accept();
        }

        public ValueTask OnJoinedActorAsync(ProbeActor actor, CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(ProbeActor actor, CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }

    private sealed class BlockingActorJoinProbe
    {
        public TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class ScopeCleanupEntrySpot(
        IZLinkEntrySpotContext context,
        BlockingScopeDependency dependency) : IZLinkEntrySpot
    {
        public IZLinkEntrySpotContext Context { get; } = context;

        private BlockingScopeDependency Dependency { get; } = dependency;
    }

    private sealed class BlockingDisposeEntrySpot(IZLinkEntrySpotContext context) : IZLinkEntrySpot
    {
        public IZLinkEntrySpotContext Context { get; } = context;

        public void Configure()
        {
            Context.Handlers.AddPacket<BlockingDisposeRouteHandler>();
        }
    }

    private sealed class BlockingDisposeRouteHandler(BlockingDisposeRouteProbe probe)
        : IZLinkSpotPacketHandler<BlockingDisposeEntrySpot, ProbeRouteMessage>
    {
        public async ValueTask HandleAsync(
            BlockingDisposeEntrySpot spot,
            ProbeRouteMessage message,
            CancellationToken cancellationToken)
        {
            probe.Started.TrySetResult();
            await probe.Release.Task.ConfigureAwait(false);
        }
    }

    private sealed class BlockingDisposeRouteProbe
    {
        public TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class BlockingScopeDependency(BlockingScopeCleanup cleanup) : IAsyncDisposable
    {
        public async ValueTask DisposeAsync()
        {
            _ = Interlocked.Increment(ref cleanup.DisposeCount);
            cleanup.Started.TrySetResult();
            await cleanup.Release.Task.ConfigureAwait(false);
            throw new InvalidOperationException("scope cleanup failed");
        }
    }

    private sealed class BlockingScopeCleanup
    {
        public TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public int DisposeCount;
    }

    private sealed class LocalEntryJoinProbe
    {
        public Func<CancellationToken, ValueTask<ZLinkSpotActorJoinResult>> Handler { get; set; } =
            _ => ValueTask.FromResult(ZLinkSpotActorJoinResult.Reject());

        public Func<IZLinkEntrySpotContext, ProbeActor, CancellationToken, ValueTask>
            JoinedHandler { get; set; } =
            static (_, _, _) => ValueTask.CompletedTask;
    }

    private sealed class LocalJoinProbeEntrySpot(
        IZLinkEntrySpotContext context,
        LocalEntryJoinProbe probe) : IZLinkEntrySpot<ProbeActor>
    {
        public IZLinkEntrySpotContext Context { get; } = context;

        public void Configure()
        {
        }

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            _ = actorId;
            _ = request;
            return probe.Handler(cancellationToken);
        }

        public ValueTask OnJoinedActorAsync(
            ProbeActor actor,
            CancellationToken cancellationToken) =>
            probe.JoinedHandler(Context, actor, cancellationToken);

        public ValueTask OnLeaveActorAsync(ProbeActor actor, CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }

    private sealed class JoinTargetSpot(
        IZLinkSpotContext context,
        FlowJoinProbe flowJoinProbe) : IZLinkSpot<ProbeActor>
    {
        public IZLinkSpotContext Context { get; } = context;

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            _ = actorId;
            _ = request;
            _ = cancellationToken;
            flowJoinProbe.JoinFlow = ZLinkFlowContext.Current;
            return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());
        }

        public ValueTask OnJoinedActorAsync(ProbeActor actor, CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(ProbeActor actor, CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }

    private sealed class CommittedJoinRetryProbe
    {
        public int Attempts;
    }

    private sealed class RetryingJoinTargetSpot(
        IZLinkSpotContext context,
        CommittedJoinRetryProbe probe) : IZLinkSpot<ProbeActor>
    {
        public IZLinkSpotContext Context { get; } = context;

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken) =>
            ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());

        public ValueTask OnJoinedActorAsync(
            ProbeActor actor,
            CancellationToken cancellationToken)
        {
            if (Interlocked.Increment(ref probe.Attempts) == 1)
                throw new InvalidOperationException("post-commit callback failed");
            return ValueTask.CompletedTask;
        }

        public ValueTask OnLeaveActorAsync(
            ProbeActor actor,
            CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }

    private sealed class ActorJoinFlowProbeSpot : IZLinkSpot<ProbeActor>
    {
        public IZLinkSpotContext Context => throw new NotSupportedException();

        public string? ObservedFlowId { get; private set; }

        public ZLinkFlowOrigin? ObservedFlowOrigin { get; private set; }

        public string? ObservedRequest { get; private set; }

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            _ = actorId;
            _ = cancellationToken;
            var flow = ZLinkFlowContext.Current;
            ObservedFlowId = flow?.FlowId;
            ObservedFlowOrigin = flow?.Origin;
            ObservedRequest = request.Decode<string>();
            return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept("join-reply"));
        }

        public ValueTask OnJoinedActorAsync(ProbeActor actor, CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(ProbeActor actor, CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }

    private sealed class TimerProbeEntrySpot(IZLinkEntrySpotContext context) : IZLinkEntrySpot
    {
        public IZLinkEntrySpotContext Context { get; } = context;

        public void Configure()
        {
            Context.Handlers.AddPacket<BlockingProbeRouteHandler>();
        }
    }

    private sealed class EntryTimerSerialProbe
    {
        public ConcurrentQueue<string> Events { get; } = new();
        public TaskCompletionSource RouteStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseRoute { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource TimerStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class BlockingProbeRouteHandler(EntryTimerSerialProbe probe)
        : IZLinkSpotPacketHandler<TimerProbeEntrySpot, ProbeRouteMessage>
    {
        public async ValueTask HandleAsync(
            TimerProbeEntrySpot spot,
            ProbeRouteMessage message,
            CancellationToken cancellationToken)
        {
            probe.Events.Enqueue("route:start");
            probe.RouteStarted.TrySetResult();
            await probe.ReleaseRoute.Task.WaitAsync(cancellationToken);
            probe.Events.Enqueue("route:end");
        }
    }

    private sealed class EntryTimerProbeHandler(EntryTimerSerialProbe probe)
        : IZLinkSpotTimerHandler<TimerProbeEntrySpot>
    {
        public ValueTask HandleAsync(
            TimerProbeEntrySpot spot,
            ZLinkTimerTick tick,
            CancellationToken cancellationToken)
        {
            probe.Events.Enqueue("timer:start");
            probe.TimerStarted.TrySetResult();
            return ValueTask.CompletedTask;
        }
    }

    private sealed record ProbeRouteMessage(string Value);

    private sealed record ProbeReply(string Value);

    private sealed class RouteDispatchProbe
    {
        public TaskCompletionSource<string> Message { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class ProbeRouteHandler(RouteDispatchProbe probe)
        : IZLinkSpotPacketHandler<ProbeEntrySpot, ProbeRouteMessage>
    {
        public ValueTask HandleAsync(
            ProbeEntrySpot spot,
            ProbeRouteMessage message,
            CancellationToken cancellationToken)
        {
            _ = spot;
            _ = cancellationToken;
            probe.Message.SetResult(message.Value);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class ProbeActorSendHandler(DispatchProbe probe)
        : IZLinkEntrySpotActorSendHandler<ProbeEntrySpot, ProbeActor, string>
    {
        public async ValueTask HandleAsync(
            ProbeEntrySpot entrySpot,
            ProbeActor actor,
            IZLinkMessageContext context,
            string message,
            CancellationToken cancellationToken)
        {
            _ = entrySpot;
            _ = context;
            probe.Events.Enqueue($"{actor.ActorId}:{message}:start");

            if (actor.ActorId == "actor-a" && message == "first")
            {
                probe.ActorAFirstStarted.SetResult();
                await probe.ReleaseActorAFirst.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
                probe.Events.Enqueue($"{actor.ActorId}:{message}:end");
                return;
            }

            if (actor.ActorId == "actor-a" && message == "second")
                probe.ActorASecondStarted.SetResult();

            if (actor.ActorId == "actor-b")
                probe.ActorBStarted.SetResult();
        }
    }

    private sealed class ProbeActorRequestHandler
        : IZLinkEntrySpotActorRequestHandler<ProbeEntrySpot, ProbeActor, string, ProbeReply>
    {
        public ValueTask<ProbeReply> HandleAsync(
            ProbeEntrySpot entrySpot,
            ProbeActor actor,
            IZLinkMessageContext context,
            string request,
            CancellationToken cancellationToken)
        {
            _ = entrySpot;
            _ = context;
            _ = cancellationToken;
            return ValueTask.FromResult(new ProbeReply($"{request}:{actor.ActorId}"));
        }
    }

    private sealed class ProbeActorFlowJoinRequestHandler(FlowJoinProbe probe)
        : IZLinkEntrySpotActorRequestHandler<ProbeEntrySpot, ProbeActor, string, ProbeReply>
    {
        public ValueTask<ProbeReply> HandleAsync(
            ProbeEntrySpot entrySpot,
            ProbeActor actor,
            IZLinkMessageContext context,
            string request,
            CancellationToken cancellationToken)
        {
            _ = entrySpot;
            _ = context;
            actor.Context.JoinSpot(probe.TargetSpotId, request).Defer();
            return ValueTask.FromResult(new ProbeReply($"{request}:{actor.ActorId}"));
        }
    }

    private sealed class FlowJoinProbe
    {
        public string TargetSpotId { get; set; } = string.Empty;

        public ZLinkFlowValue? JoinFlow { get; set; }
    }

    private sealed class ProbeActorDestroyRequestHandler
        : IZLinkEntrySpotActorRequestHandler<ProbeEntrySpot, ProbeActor, string, ProbeReply>
    {
        public async ValueTask<ProbeReply> HandleAsync(
            ProbeEntrySpot entrySpot,
            ProbeActor actor,
            IZLinkMessageContext context,
            string request,
            CancellationToken cancellationToken)
        {
            _ = context;
            await entrySpot.Context.DestroyActorAsync(actor, cancellationToken);
            return new ProbeReply($"{request}:{actor.ActorId}");
        }
    }

    private sealed class ProbeActorThrowingRequestHandler
        : IZLinkEntrySpotActorRequestHandler<ProbeEntrySpot, ProbeActor, string, ProbeReply>
    {
        public ValueTask<ProbeReply> HandleAsync(
            ProbeEntrySpot entrySpot,
            ProbeActor actor,
            IZLinkMessageContext context,
            string request,
            CancellationToken cancellationToken)
        {
            _ = entrySpot;
            _ = actor;
            _ = context;
            _ = cancellationToken;
            throw new InvalidOperationException($"boom:{request}");
        }
    }

    private sealed record CapturedActorReply(
        ZLinkBackendActorRef Actor,
        RoutingId SourceNodeRid,
        RoutingId SourceSessionRid,
        ulong RequestId,
        uint Flags,
        IReadOnlyList<byte[]> Parts);

    private sealed class CapturingSpot :
        IZLinkBackendSpot,
        IZLinkBackendAuthorityObserver
    {
        public ulong LifecycleGeneration { get; set; } = 1;

        private Action<ZLinkBackendSpotDispatchInfo>? _dispatchHandler;

        public RoutingId RoutingId { get; private set; } = RoutingId.From("entry-spot");

        public Action<RoutingId>? RoutingIdChanged { get; set; }

        public Func<ValueTask>? DisposeHandler { get; set; }

        public int DisposeCount { get; private set; }

        public ZLinkEnvelopeHeader? PublishedHeader { get; private set; }

        public int? ActorJoinResultCode { get; private set; }

        public ZLinkEnvelopeHeader? ActorJoinReplyHeader { get; private set; }

        public byte[]? ActorJoinReplyMessageBytes { get; private set; }

        public SubmitResult SpotSendResult { get; set; } = SubmitResult.Backpressured;

        public Func<IReadOnlyList<Message>, IReadOnlyList<Message>>? SpotRequestHandler { get; set; }

        public ConcurrentQueue<RequestResult> SpotRequestResults { get; } = new();

        public List<(RoutingId NodeRid, string SpotId, ulong Generation)> SpotSends { get; } = [];

        public List<(RoutingId NodeRid, string SpotId, ulong Generation)> SpotRequests { get; } = [];

        public ulong LocalOwnerLeaseGeneration { get; private set; }

        public List<(
            ZLinkBackendActorRef Actor,
            ulong TargetNodeGeneration,
            ulong AuthorityOwnerGeneration,
            ulong OwnerLeaseGeneration)> ObservedActorAuthorities { get; } = [];

        public List<(
            RoutingId NodeRid,
            string SpotId,
            ulong ObjectGeneration,
            ulong TargetNodeGeneration,
            ulong AuthorityOwnerGeneration,
            ulong OwnerLeaseGeneration)> ObservedSpotAuthorities { get; } = [];

        public void SetLocalOwnerLeaseGeneration(ulong ownerLeaseGeneration) =>
            LocalOwnerLeaseGeneration = ownerLeaseGeneration;

        public void ObserveActorAuthority(
            ZLinkBackendActorRef actor,
            ulong targetNodeGeneration,
            ulong authorityOwnerGeneration,
            ulong ownerLeaseGeneration) =>
            ObservedActorAuthorities.Add((
                actor,
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration));

        public void ObserveSpotAuthority(
            RoutingId nodeRid,
            string spotId,
            ulong objectGeneration,
            ulong targetNodeGeneration,
            ulong authorityOwnerGeneration,
            ulong ownerLeaseGeneration) =>
            ObservedSpotAuthorities.Add((
                nodeRid,
                spotId,
                objectGeneration,
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration));

        public ValueTask DisposeAsync()
        {
            DisposeCount++;
            return DisposeHandler is { } handler ? handler() : ValueTask.CompletedTask;
        }

        public void SetRoutingId(RoutingId routingId)
        {
            RoutingId = routingId;
            RoutingIdChanged?.Invoke(routingId);
        }

        public void SetSubscription(string channelName, string topic) { }

        public ZLinkBackendSubscribeMessage? Subscribe(RecvFlags flags) => null;

        public ZLinkBackendRouteReceived? RecvRoute(RecvFlags flags) => null;

        public void OnDispatchEvent(Action<ZLinkBackendSpotDispatchInfo> handler)
        {
            _dispatchHandler = handler;
        }

        public bool DispatchHandlerAttached => _dispatchHandler is not null;

        public void RaiseDispatch(ZLinkBackendSpotDispatchInfo info)
        {
            (_dispatchHandler ?? throw new InvalidOperationException("Dispatch handler was not attached.")).Invoke(info);
        }

        public bool RequestToChannel(
            string channelName,
            Message message,
            ZLinkBackendRequestCallback callback,
            SendFlags flags,
            TimeSpan? timeout,
            ReadOnlyMemory<byte> metadata) => false;

        public bool RequestToChannel(
            string channelName,
            IReadOnlyList<Message> parts,
            ZLinkBackendRequestCallback callback,
            SendFlags flags,
            TimeSpan? timeout,
            ReadOnlyMemory<byte> metadata) => false;

        public SubmitResult SendToChannel(
            string channelName, Message message, SendFlags flags,
            ReadOnlyMemory<byte> metadata) => SubmitResult.Backpressured;

        public SubmitResult SendToChannel(
            string channelName, IReadOnlyList<Message> parts, SendFlags flags,
            ReadOnlyMemory<byte> metadata) => SubmitResult.Backpressured;

        public void Publish(
            string channelName, string topic, Message message, SendFlags flags,
            ReadOnlyMemory<byte> metadata)
        {
            _ = topic;
            _ = flags;
            PublishedHeader = ZLinkEnvelopeCodec.DecodeHeader(message);
        }

        public void Publish(
            string channelName, string topic, IReadOnlyList<Message> parts, SendFlags flags,
            ReadOnlyMemory<byte> metadata)
        {
            _ = topic;
            _ = flags;
            PublishedHeader = ZLinkEnvelopeCodec.DecodeHeader(parts);
        }

        public SubmitResult SendToSpot(
            RoutingId targetRid, string targetSpotId, ulong spotGeneration,
            Message message, SendFlags flags, ReadOnlyMemory<byte> metadata)
        {
            SpotSends.Add((targetRid, targetSpotId, spotGeneration));
            return SpotSendResult;
        }

        public SubmitResult SendToSpot(
            RoutingId targetRid,
            string targetSpotId,
            ulong spotGeneration,
            IReadOnlyList<Message> parts,
            SendFlags flags,
            ReadOnlyMemory<byte> metadata)
        {
            SpotSends.Add((targetRid, targetSpotId, spotGeneration));
            return SpotSendResult;
        }

        public ValueTask SendToSpotAsync(
            RoutingId targetRid,
            string targetSpotId,
            ulong spotGeneration,
            IReadOnlyList<Message> parts,
            SendFlags flags,
            CancellationToken cancellationToken,
            ReadOnlyMemory<byte> metadata = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var result = SendToSpot(
                targetRid,
                targetSpotId,
                spotGeneration,
                parts,
                flags,
                metadata);
            return result == SubmitResult.Ok
                ? ValueTask.CompletedTask
                : ValueTask.FromException(new ZlinkSubmitException(
                    (ZlinkSubmitException.ErrorCode)(int)result));
        }

        public ValueTask<IReadOnlyList<Message>> RequestToSpotAsync(
            RoutingId targetRid,
            string targetSpotId,
            ulong spotGeneration,
            IReadOnlyList<Message> parts,
            SendFlags flags,
            TimeSpan timeout,
            CancellationToken cancellationToken,
            ReadOnlyMemory<byte> metadata = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            _ = flags;
            _ = timeout;
            _ = metadata;
            if (SpotRequestHandler is null)
                return ValueTask.FromException<IReadOnlyList<Message>>(
                    new ZlinkSubmitException(
                        ZlinkSubmitException.ErrorCode.NotConnected));
            SpotRequests.Add((targetRid, targetSpotId, spotGeneration));
            var result = SpotRequestResults.TryDequeue(out var configured)
                ? configured
                : RequestResult.Ok;
            return result == RequestResult.Ok
                ? ValueTask.FromResult(SpotRequestHandler(parts))
                : ValueTask.FromException<IReadOnlyList<Message>>(
                    new ZlinkRequestException(
                        (ZlinkRequestException.ErrorCode)(int)result));
        }

        public bool RequestToSpot(
            RoutingId targetRid,
            string targetSpotId,
            ulong spotGeneration,
            Message message,
            ZLinkBackendRequestCallback callback,
            SendFlags flags,
            TimeSpan? timeout,
            ReadOnlyMemory<byte> metadata)
        {
            if (SpotRequestHandler is null)
                return false;
            SpotRequests.Add((targetRid, targetSpotId, spotGeneration));
            var result = SpotRequestResults.TryDequeue(out var configured)
                ? configured
                : RequestResult.Ok;
            callback(
                result,
                result == RequestResult.Ok
                    ? SpotRequestHandler([message])
                    : []);
            return true;
        }

        public bool RequestToSpot(
            RoutingId targetRid,
            string targetSpotId,
            ulong spotGeneration,
            IReadOnlyList<Message> parts,
            ZLinkBackendRequestCallback callback,
            SendFlags flags,
            TimeSpan? timeout,
            ReadOnlyMemory<byte> metadata)
        {
            if (SpotRequestHandler is null)
                return false;
            SpotRequests.Add((targetRid, targetSpotId, spotGeneration));
            var result = SpotRequestResults.TryDequeue(out var configured)
                ? configured
                : RequestResult.Ok;
            callback(
                result,
                result == RequestResult.Ok
                    ? SpotRequestHandler(parts)
                    : []);
            return true;
        }

        public ZLinkBackendActorJoinRequest? RecvActorJoin(RecvFlags flags) => null;

        public ZLinkBackendSpotActorLifecycleEvent? RecvActorLifecycle(RecvFlags flags) => null;

        public void ReplyActorJoin(
            ZLinkBackendActorJoinRequest request,
            int joinResultCode,
            Message reply)
        {
            _ = request;
            ActorJoinResultCode = joinResultCode;
            ActorJoinReplyHeader = null;
            ActorJoinReplyMessageBytes = reply.AsReadOnlyMemory().ToArray();
        }

        public void ReplyActorJoin(
            ZLinkBackendActorJoinRequest request,
            int joinResultCode,
            IReadOnlyList<Message> parts)
        {
            _ = request;
            ActorJoinResultCode = joinResultCode;
            ActorJoinReplyHeader = ZLinkEnvelopeCodec.DecodeHeader(parts);
        }

        public void OnActorLifecycle(
            Action<ZLinkBackendSpotActorLifecycleInfo>? onJoin,
            Action<ZLinkBackendSpotActorLifecycleInfo>? onLeave) { }
    }

    private sealed class RelaySessionHandlerRegistry : IZLinkSessionHandlerRegistry
    {
        public void AddHandler<THandler>() where THandler : class { }

        public void AddHandler<THandler>(string packetName) where THandler : class { }

        public ValueTask<bool> TryHandleAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(false);
    }

    private sealed class CountingStreamSocket : IZLinkBackendStreamSocket
    {
        private int _sendCount;

        internal int SendCount => Volatile.Read(ref _sendCount);

        public void Bind(string endpoint) { }

        public void SetTlsServer(
            string certPath,
            string keyPath,
            bool requireClientCert) { }

        public bool RecvPacket(
            out ZLinkBackendStreamReceive? received,
            RecvFlags flags = RecvFlags.None)
        {
            received = null;
            return false;
        }

        public bool Send(
            RoutingId routingId,
            Message payload,
            SendFlags flags)
        {
            Interlocked.Increment(ref _sendCount);
            return true;
        }

        public bool Send(
            RoutingId routingId,
            IReadOnlyList<Message> parts,
            SendFlags flags)
        {
            Interlocked.Increment(ref _sendCount);
            return true;
        }

        public void DisconnectPeer(RoutingId routingId) { }

        public ValueTask BindActorAsync(
            RoutingId sessionRid,
            ZLinkBackendActorRef actor,
            TimeSpan timeout,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask UnbindActorAsync(
            RoutingId sessionRid,
            string actorId,
            TimeSpan timeout,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public bool SendBoundActor(
            RoutingId sessionRid,
            string actorId,
            IReadOnlyList<Message> parts,
            SendFlags flags)
        {
            Interlocked.Increment(ref _sendCount);
            return true;
        }

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    private sealed class RelayStreamSocket : IZLinkBackendStreamSocket
    {
        public void Bind(string endpoint) { }

        public void SetTlsServer(
            string certPath,
            string keyPath,
            bool requireClientCert) { }

        public bool RecvPacket(
            out ZLinkBackendStreamReceive? received,
            RecvFlags flags = RecvFlags.None)
        {
            received = null;
            return false;
        }

        public bool Send(
            RoutingId routingId,
            Message payload,
            SendFlags flags) => true;

        public bool Send(
            RoutingId routingId,
            IReadOnlyList<Message> parts,
            SendFlags flags) => true;

        public void DisconnectPeer(RoutingId routingId) { }

        public ValueTask BindActorAsync(
            RoutingId sessionRid,
            ZLinkBackendActorRef actor,
            TimeSpan timeout,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask UnbindActorAsync(
            RoutingId sessionRid,
            string actorId,
            TimeSpan timeout,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public bool SendBoundActor(
            RoutingId sessionRid,
            string actorId,
            IReadOnlyList<Message> parts,
            SendFlags flags) => true;

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    private sealed class DelayedLocationProviderStore(
        IZLinkLocationStore inner,
        TimeSpan delay) : IZLinkLocationStore
    {
        public async ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default)
        {
            if (key.Value.StartsWith("authority\0actor\0", StringComparison.Ordinal))
                await Task.Delay(delay, cancellationToken);
            return await inner.ReadAsync(key, cancellationToken);
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
    private sealed class CapturingSpotNode :
        IZLinkBackendSpotNode,
        IZLinkBackendAuthorityObserver,
        IZLinkBackendRequestSourceFenceObserver,
        IZLinkBackendLocalActorAuthorityReader
    {
        private readonly CapturingSpot _entrySpot = new();
        private long _nextOperationId;

        public CapturingSpotNode()
        {
            _entrySpot.RoutingIdChanged = routingId =>
                InitializationEvents.Enqueue($"entry-rid:{routingId}");
        }

        public ConcurrentQueue<string> InitializationEvents { get; } = new();

        public ulong LocalOwnerLeaseGeneration { get; private set; }

        public List<(
            ZLinkBackendActorRef Actor,
            ulong TargetNodeGeneration,
            ulong AuthorityOwnerGeneration,
            ulong OwnerLeaseGeneration)> ObservedActorAuthorities { get; } = [];

        public List<MeshNodePeer> AdmittedMeshPeers { get; } = [];

        private readonly Dictionary<ZLinkBackendActorRef, (ulong Authority, ulong Lease)>
            _localActorAuthorities = [];

        public List<(
            RoutingId NodeRid,
            string SpotId,
            ulong ObjectGeneration,
            ulong TargetNodeGeneration,
            ulong AuthorityOwnerGeneration,
            ulong OwnerLeaseGeneration)> ObservedSpotAuthorities { get; } = [];

        public RoutingId EntryRoutingId => _entrySpot.RoutingId;

        public MeshOperationId AllocateOperationId() =>
            new(1, checked((ulong)Interlocked.Increment(ref _nextOperationId)));

        public RoutingId PublisherRoutingId { get; private set; }

        public IZLinkSpotPublisherConfig? PublisherConfig { get; private set; }

        public IZLinkSpotSubscriberConfig? SubscriberConfig { get; private set; }

        public ulong RouterHighWaterMark { get; private set; }

        public TimeSpan? RouterSendTimeout { get; private set; }

        public TimeSpan? LastActorRequestTimeout { get; private set; }

        public CapturingSpot EntrySpotBackend => _entrySpot;

        public List<CapturingSpot> CreatedSpots { get; } = [];

        public Func<CapturingSpot>? CreatedSpotFactory { get; set; }

        public IActorCreateOperationTarget? ActorCreateOperationTarget { get; private set; }

        public IActorDestroyOperationTarget? ActorDestroyOperationTarget { get; private set; }

        public IUserSpotOperationTarget? UserSpotOperationTarget { get; private set; }

        public Func<ValueTask>? DisposeHandler
        {
            get => _entrySpot.DisposeHandler;
            set => _entrySpot.DisposeHandler = value;
        }

        public List<CapturedActorReply> NoBindReplies { get; } = [];

        public bool NoBindReplyAccepted { get; set; } = true;

        public List<(ZLinkBackendActorRef Actor, IReadOnlyList<byte[]> Parts)> BoundSessionReplies { get; } = [];

        public bool BoundSessionSendAccepted { get; set; } = true;

        public int BoundSessionSendAttempts { get; private set; }

        public ulong LastBoundSessionBindingGeneration { get; private set; }

        private TaskCompletionSource? BoundSessionAdmission { get; set; }

        public void CompleteBoundSessionAdmission()
        {
            if (BoundSessionSendAccepted)
                BoundSessionAdmission?.TrySetResult();
        }

        public void SetActorCreateOperationTarget(IActorCreateOperationTarget target)
        {
            ActorCreateOperationTarget = target;
            InitializationEvents.Enqueue("actor-create-target");
        }

        public void SetActorDestroyOperationTarget(IActorDestroyOperationTarget target) =>
            ActorDestroyOperationTarget = target;

        public void SetUserSpotOperationTarget(IUserSpotOperationTarget target)
        {
            UserSpotOperationTarget = target;
            InitializationEvents.Enqueue("user-spot-target");
        }

        public void SetLocalOwnerLeaseGeneration(ulong ownerLeaseGeneration) =>
            LocalOwnerLeaseGeneration = ownerLeaseGeneration;

        public void SetLocalRequestSourceFence(
            ZLinkServiceWireCodec.RequestSourceFence source) { }

        public void ObserveRequestSourceFence(
            ZLinkServiceWireCodec.RequestSourceFence source) { }

        public void ObserveActorAuthority(
            ZLinkBackendActorRef actor,
            ulong targetNodeGeneration,
            ulong authorityOwnerGeneration,
            ulong ownerLeaseGeneration) =>
            ObservedActorAuthorities.Add((
                actor,
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration));

        public void ObserveSpotAuthority(
            RoutingId nodeRid,
            string spotId,
            ulong objectGeneration,
            ulong targetNodeGeneration,
            ulong authorityOwnerGeneration,
            ulong ownerLeaseGeneration) =>
            ObservedSpotAuthorities.Add((
                nodeRid,
                spotId,
                objectGeneration,
                targetNodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration));

        public ZLinkBackendActorJoinEntrySpotResult? EntrySpotJoinResult { get; set; }

        public IReadOnlyList<Message> EntrySpotJoinReplyParts { get; set; } = [];

        public Exception? NodeRequestFailure { get; set; }

        public RoutingId LastNodeSendTarget { get; private set; }

        public SendFlags LastNodeSendFlags { get; private set; }

        public byte[] LastNodeSendMetadata { get; private set; } = [];

        public IReadOnlyList<byte[]> LastNodeSendParts { get; private set; } = [];

        public ConcurrentQueue<SubmitResult> NodeSendResults { get; } = new();

        public List<IReadOnlyList<byte[]>> NodeSendAttempts { get; } = [];

        public int NodeSendAsyncCalls { get; private set; }

        public RoutingId LastNodeRequestTarget { get; private set; }

        public SendFlags LastNodeRequestFlags { get; private set; }

        public byte[] LastNodeRequestMetadata { get; private set; } = [];

        public Func<IReadOnlyList<Message>, (ZLinkBackendActorJoinResult Result, IReadOnlyList<Message> Reply)>?
            ActorJoinHandler { get; set; }

        public bool DeferActorJoinCallback { get; set; }

        public ActorJoinCallback? DeferredActorJoinCallback { get; private set; }

        public Exception? ActorJoinSubmitFailure { get; set; }

        public IReadOnlyList<Message>? ActorJoinSubmittedParts { get; private set; }

        public bool DeferEntrySpotJoinCallback { get; set; }

        public ActorJoinEntrySpotCallback? DeferredEntrySpotJoinCallback { get; private set; }

        public RoutingId LastActorJoinTargetNodeRid { get; private set; }

        public string LastActorJoinTargetSpotId { get; private set; } = string.Empty;

        public SendFlags LastActorSendFlags { get; private set; }

        public bool ActorSendAccepted { get; set; }

        public SubmitResult? ActorSendResult { get; set; }

        public List<(ZLinkBackendActorRef Actor, IReadOnlyList<byte[]> Parts)> ActorSends { get; } = [];

        public SubmitResult SpotSendResult
        {
            get => _entrySpot.SpotSendResult;
            set => _entrySpot.SpotSendResult = value;
        }

        public Func<IReadOnlyList<Message>, IReadOnlyList<Message>>? SpotRequestHandler
        {
            get => _entrySpot.SpotRequestHandler;
            set => _entrySpot.SpotRequestHandler = value;
        }

        public ConcurrentQueue<RequestResult> SpotRequestResults =>
            _entrySpot.SpotRequestResults;

        public IReadOnlyList<(RoutingId NodeRid, string SpotId, ulong Generation)> SpotSends =>
            _entrySpot.SpotSends;

        public IReadOnlyList<(RoutingId NodeRid, string SpotId, ulong Generation)> SpotRequests =>
            _entrySpot.SpotRequests;

        public List<(ZLinkBackendActorRef Actor, IReadOnlyList<byte[]> Parts)> ActorRequests { get; } = [];

        public Func<IReadOnlyList<Message>, IReadOnlyList<Message>>? ActorRequestHandler { get; set; }

        public List<ZLinkBackendActorRef> DestroyedActors { get; } = [];

        public ConcurrentQueue<string> LifecycleEvents { get; } = new();

        public List<ZLinkBackendActorRef> CreatedActors { get; } = [];

        public ZLinkBackendActorRef? ActorLookupResult { get; set; }

        public List<RoutingId> CreatedActorEntryRids { get; } = [];

        public Action<ZLinkBackendActorRef>? BeforeDestroy { get; set; }

        public Action<ZLinkBackendActorRef>? BeforeNoBindReply { get; set; }

        public Exception? DestroyFailure { get; set; }

        public Func<ZLinkBackendActorRef, CancellationToken, ValueTask>? DestroyHandler { get; set; }

        public ConcurrentQueue<bool> ForwardResults { get; } = new();

        public Action? BeforeForwardActorBoundSessionPart { get; set; }

        public List<(bool HasMore, byte[] Payload)> MessageFollowParts { get; } = [];

        public TaskCompletionSource FinalPartAttempted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public RoutingId RoutingId { get; private set; }

        public Func<ValueTask>? NodeDisposeHandler { get; set; }

        public ValueTask DisposeAsync() =>
            NodeDisposeHandler?.Invoke() ?? ValueTask.CompletedTask;

        public void SetRoutingId(RoutingId routingId)
        {
            RoutingId = routingId;
            InitializationEvents.Enqueue($"node-rid:{routingId}");
        }

        public void SetObjectRole(ZLinkMeshNodeObjectRole objectRole)
        {
            InitializationEvents.Enqueue($"object-role:{objectRole}");
        }

        public void SetPublisherRoutingId(RoutingId routingId)
        {
            PublisherRoutingId = routingId;
        }

        public void SetSubscriberRoutingId(RoutingId routingId) { }

        public void SetRouterBind(string endpoint)
        {
            InitializationEvents.Enqueue($"router-bind:{endpoint}");
        }

        public void SetRouterAdvertisedEndpoint(string endpoint) { }

        public void SetPubBind(string endpoint) { }

        public void SetMailboxBudgets(ulong messageBudget, ulong byteBudget) { }

        public void SetRouterHighWaterMark(ulong value)
        {
            RouterHighWaterMark = value;
        }

        public void SetRouterSendTimeout(TimeSpan? value)
        {
            RouterSendTimeout = value;
        }

        public void ApplyRoleConfig(
            IZLinkSpotPublisherConfig? publisher,
            IZLinkSpotSubscriberConfig? subscriber)
        {
            PublisherConfig = publisher;
            SubscriberConfig = subscriber;
        }

        public void ConnectPeer(string endpoint) { }

        public void ConnectPeer(
            RoutingId peerRid,
            string endpoint,
            string expectedSecurityIdentity) { }

        public void SetPeerExpectation(
            RoutingId peerRid,
            string endpoint,
            string expectedSecurityIdentity,
            ulong expectedLifecycleGeneration) { }

        public void RemovePeerExpectation(RoutingId peerRid, string endpoint) { }

        public void DisconnectPeer(string endpoint) { }

        public void DisconnectPeerLifetime(RoutingId peerRid, ulong lifecycleGeneration) { }

        public IZLinkBackendSpot CreateSpot()
        {
            var spot = CreatedSpotFactory?.Invoke() ?? new CapturingSpot();
            spot.SetRoutingId(PublisherRoutingId);
            CreatedSpots.Add(spot);
            return spot;
        }

        public IZLinkBackendSpot GetOrCreateSpot(string targetSpotId, out bool created)
        {
            var lifecycleGeneration = BeforeGetOrCreateSpot?.Invoke(targetSpotId);
            created = true;
            var spot = CreatedSpotFactory?.Invoke() ?? new CapturingSpot();
            if (lifecycleGeneration is { } generation && spot is CapturingSpot capturing)
                capturing.LifecycleGeneration = generation;
            spot.SetRoutingId(RoutingId.From(targetSpotId));
            CreatedSpots.Add(spot);
            return spot;
        }

        public Func<string, ulong>? BeforeGetOrCreateSpot { get; set; }

        public IZLinkBackendSpot GetOrCreateReservedSpot(
            string targetSpotId,
            ulong objectGeneration,
            ulong authorityOwnerGeneration,
            out bool created)
        {
            _ = objectGeneration;
            _ = authorityOwnerGeneration;
            return GetOrCreateSpot(targetSpotId, out created);
        }

        public void SetLocalActorAuthority(
            ZLinkBackendActorRef actor,
            ulong authorityOwnerGeneration)
        {
            if (!_localActorAuthorities.TryGetValue(actor, out var current))
                current = default;
            _localActorAuthorities[actor] = (
                authorityOwnerGeneration,
                current.Lease == 0 ? LocalOwnerLeaseGeneration : current.Lease);
        }

        public void SetLocalActorAuthorityFence(
            ZLinkBackendActorRef actor,
            ulong authorityOwnerGeneration,
            ulong ownerLeaseGeneration) =>
            _localActorAuthorities[actor] = (
                authorityOwnerGeneration,
                ownerLeaseGeneration);

        public Action<int, ZLinkBackendActorRef>? AfterLocalActorAuthorityRead
            { get; set; }

        private int _localActorAuthorityReadCount;

        public bool TryGetLocalActorAuthority(
            ZLinkBackendActorRef actor,
            out ulong authorityOwnerGeneration,
            out ulong ownerLeaseGeneration)
        {
            if (_localActorAuthorities.TryGetValue(actor, out var authority))
            {
                authorityOwnerGeneration = authority.Authority;
                ownerLeaseGeneration = authority.Lease;
                AfterLocalActorAuthorityRead?.Invoke(
                    Interlocked.Increment(ref _localActorAuthorityReadCount),
                    actor);
                return authorityOwnerGeneration != 0 && ownerLeaseGeneration != 0;
            }
            authorityOwnerGeneration = 0;
            ownerLeaseGeneration = 0;
            return false;
        }

        public ZLinkSpotNodeStatus Status() => new(
            "entry",
            "inproc://entry",
            RoutingId,
            ZLinkSpotNodeState.Ready,
            0,
            0,
            0,
            0,
            0,
            0,
            0);

        public IReadOnlyList<ZLinkSpotNodePeerEntry> Peers() => [];

        public MeshNodeStatus MeshStatus() => new(
            MeshNodeState.Ready,
            RoutingId,
            "entry",
            "inproc://entry",
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0);

        public IReadOnlyList<MeshNodePeer> MeshPeers() => AdmittedMeshPeers;

        public IReadOnlyList<MeshPeerChannel> MeshPeerChannels(
            RoutingId peerRid,
            ulong lifecycleGeneration) => [];

        public IMeshNodeMonitor OpenMeshMonitor(
            MeshMonitorEventMask events = MeshMonitorEventMask.All) =>
            new UnusedMeshNodeMonitor();

        public IReadOnlyList<ZLinkSpotNodeSubjectEntry> Subjects() => [];

        public List<string> AddedChannels { get; } = [];

        public Dictionary<string, uint> ChannelWeights { get; } = new();

        public int StartCount { get; private set; }

        public Action? NativeIngressOnStart { get; set; }

        public Action? ApplicationIngressOnActivation { get; set; }

        public bool ActorTargetReadyAtStart { get; private set; }

        public bool UserSpotTargetReadyAtStart { get; private set; }

        public bool NodeRouteReadyAtStart { get; private set; }

        public bool EntryDispatchReadyAtActivation { get; private set; }

        public Action<ZLinkBackendRouteReceived>? NodeRouteHandler { get; private set; }

        public void AddChannel(string channelName) => AddedChannels.Add(channelName);

        public void SetChannelWeight(string channelName, uint weight) =>
            ChannelWeights[channelName] = weight;

        public bool DrainingPublished { get; private set; }

        public void PublishDraining() => DrainingPublished = true;

        public void SetMaxMessageSize(long value)
        {
        }

        public void Start()
        {
            StartCount++;
            ActorTargetReadyAtStart = ActorCreateOperationTarget is not null;
            UserSpotTargetReadyAtStart = UserSpotOperationTarget is not null;
            NodeRouteReadyAtStart = NodeRouteHandler is not null;
            InitializationEvents.Enqueue("node-start");
            NativeIngressOnStart?.Invoke();
        }

        public void ActivateIngress()
        {
            EntryDispatchReadyAtActivation = _entrySpot.DispatchHandlerAttached;
            InitializationEvents.Enqueue("ingress-activate");
            ApplicationIngressOnActivation?.Invoke();
        }

        public void OnNodeRoute(Action<ZLinkBackendRouteReceived> handler)
        {
            NodeRouteHandler = handler;
            InitializationEvents.Enqueue("node-route-handler");
        }

        private sealed class UnusedMeshNodeMonitor : IMeshNodeMonitor
        {
            public MeshMonitorEvent? Recv(RecvFlags flags = RecvFlags.None) =>
                throw new NotSupportedException();

            public MeshMonitorStatus Status() => throw new NotSupportedException();

            public void Close() { }

            public void Dispose() { }

            public ValueTask DisposeAsync() => ValueTask.CompletedTask;
        }

        public IZLinkBackendSpot EntrySpot()
        {
            InitializationEvents.Enqueue("entry-facade");
            return _entrySpot;
        }

        public ZLinkBackendActorRef CreateActor(string actorId, Message createRequest)
        {
            var actor = new ZLinkBackendActorRef(RoutingId, actorId, 1);
            CreatedActors.Add(actor);
            CreatedActorEntryRids.Add(_entrySpot.RoutingId);
            return actor;
        }

        public ZLinkBackendActorRef? ActorLookup(string actorId) => ActorLookupResult;

        public bool JoinActor(
            ZLinkBackendActorRef actor,
            RoutingId destNodeRid,
            string destSpotId,
            Message message,
            ZLinkBackendRequestCallback callback,
            TimeSpan? timeout) => false;

        public bool JoinActor(
            ZLinkBackendActorRef actor,
            RoutingId destNodeRid,
            string destSpotId,
            IReadOnlyList<Message> parts,
            ActorJoinCallback callback,
            TimeSpan? timeout)
        {
            _ = actor;
            _ = timeout;
            ActorJoinSubmittedParts = parts;
            if (ActorJoinSubmitFailure is { } failure) throw failure;
            if (DeferActorJoinCallback)
            {
                LastActorJoinTargetNodeRid = destNodeRid;
                LastActorJoinTargetSpotId = destSpotId;
                DeferredActorJoinCallback = callback;
                return true;
            }
            if (ActorJoinHandler is null) return false;
            LastActorJoinTargetNodeRid = destNodeRid;
            LastActorJoinTargetSpotId = destSpotId;
            var (result, reply) = ActorJoinHandler(parts);
            callback(result, reply);
            return true;
        }

        public bool JoinActorEntrySpot(
            ZLinkBackendActorRef actor,
            RoutingId destNodeRid,
            Message request,
            ActorJoinEntrySpotCallback callback,
            TimeSpan? timeout)
        {
            if (DeferEntrySpotJoinCallback)
            {
                DeferredEntrySpotJoinCallback = callback;
                return true;
            }
            if (EntrySpotJoinResult is not { } result) return false;

            callback(result, EntrySpotJoinReplyParts);
            return true;
        }

        public async ValueTask DestroyActorAsync(
            ZLinkBackendActorRef actor,
            TimeSpan timeout,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            BeforeDestroy?.Invoke(actor);
            DestroyedActors.Add(actor);
            if (DestroyFailure is { } failure) throw failure;
            if (DestroyHandler is { } handler)
                await handler(actor, cancellationToken);
        }

        public SubmitResult SendToNode(
            RoutingId targetNodeRid,
            IReadOnlyList<Message> parts,
            SendFlags flags)
        {
            return SendToNode(targetNodeRid, parts, flags, default);
        }

        public SubmitResult SendToNode(
            RoutingId targetNodeRid,
            IReadOnlyList<Message> parts,
            SendFlags flags,
            ReadOnlyMemory<byte> metadata)
        {
            LastNodeSendTarget = targetNodeRid;
            LastNodeSendFlags = flags;
            LastNodeSendMetadata = metadata.ToArray();
            var copied = CopyParts(parts);
            LastNodeSendParts = copied;
            NodeSendAttempts.Add(copied);
            return NodeSendResults.TryDequeue(out var result)
                ? result
                : SubmitResult.Ok;
        }

        public async ValueTask SendToNodeAsync(
            RoutingId targetNodeRid,
            IReadOnlyList<Message> parts,
            SendFlags flags,
            CancellationToken cancellationToken,
            ReadOnlyMemory<byte> metadata = default)
        {
            NodeSendAsyncCalls++;
            while (true)
            {
                cancellationToken.ThrowIfCancellationRequested();
                var result = SendToNode(targetNodeRid, parts, flags, metadata);
                if (result == SubmitResult.Ok) return;
                if (result != SubmitResult.Backpressured)
                    throw new ZlinkSubmitException(
                        (ZlinkSubmitException.ErrorCode)(int)result);
                await Task.Yield();
            }
        }

        public bool RequestToNode(
            RoutingId targetNodeRid,
            IReadOnlyList<Message> parts,
            ZLinkBackendRequestCallback callback,
            SendFlags flags,
            TimeSpan timeout,
            ReadOnlyMemory<byte> metadata = default)
        {
            LastNodeRequestTarget = targetNodeRid;
            _ = parts;
            _ = callback;
            LastNodeRequestFlags = flags;
            _ = timeout;
            LastNodeRequestMetadata = metadata.ToArray();
            throw NodeRequestFailure
                  ?? new NotSupportedException("No node request result was configured for this test.");
        }

        public ValueTask<IReadOnlyList<Message>> RequestToNodeAsync(
            RoutingId targetNodeRid,
            IReadOnlyList<Message> parts,
            SendFlags flags,
            TimeSpan timeout,
            CancellationToken cancellationToken,
            ReadOnlyMemory<byte> metadata = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            LastNodeRequestTarget = targetNodeRid;
            _ = parts;
            LastNodeRequestFlags = flags;
            _ = timeout;
            LastNodeRequestMetadata = metadata.ToArray();
            return ValueTask.FromException<IReadOnlyList<Message>>(
                NodeRequestFailure
                ?? new NotSupportedException(
                    "No node request result was configured for this test."));
        }

        public bool SendActorBoundSession(
            ZLinkBackendActorRef actor,
            IReadOnlyList<Message> parts,
            SendFlags flags)
        {
            BoundSessionSendAttempts++;
            if (BoundSessionSendAccepted)
                BoundSessionReplies.Add((actor, CopyParts(parts)));
            return BoundSessionSendAccepted;
        }

        public async ValueTask SendActorBoundSessionAsync(
            ZLinkBackendActorRef actor,
            ulong expectedBindingGeneration,
            IReadOnlyList<Message> parts,
            CancellationToken cancellationToken)
        {
            BoundSessionSendAttempts++;
            LastBoundSessionBindingGeneration = expectedBindingGeneration;
            if (!BoundSessionSendAccepted)
            {
                var admission = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                BoundSessionAdmission = admission;
                await admission.Task.WaitAsync(cancellationToken)
                    .ConfigureAwait(false);
            }
            BoundSessionReplies.Add((actor, CopyParts(parts)));
        }

        public SubmitResult SendToActor(
            ZLinkBackendActorRef actor,
            IReadOnlyList<Message> parts,
            SendFlags flags)
        {
            LastActorSendFlags = flags;
            ActorSends.Add((actor, CopyParts(parts)));
            return ActorSendResult
                ?? (ActorSendAccepted ? SubmitResult.Ok : SubmitResult.Backpressured);
        }

        public ValueTask SendToActorAsync(
            ZLinkBackendActorRef actor,
            IReadOnlyList<Message> parts,
            SendFlags flags,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var result = SendToActor(actor, parts, flags);
            return result == SubmitResult.Ok
                ? ValueTask.CompletedTask
                : ValueTask.FromException(new ZlinkSubmitException(
                    (ZlinkSubmitException.ErrorCode)(int)result));
        }

        public ValueTask<IReadOnlyList<Message>> RequestToActorAsync(
            ZLinkBackendActorRef actor,
            IReadOnlyList<Message> parts,
            TimeSpan? timeout,
            CancellationToken cancellationToken)
        {
            LastActorRequestTimeout = timeout;
            cancellationToken.ThrowIfCancellationRequested();
            ActorRequests.Add((actor, CopyParts(parts)));
            return ValueTask.FromResult(
                ActorRequestHandler?.Invoke(parts)
                ?? throw new NotSupportedException());
        }

        public bool ReplyActorNoBind(
            ZLinkBackendActorRef actor,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid,
            ulong requestId,
            uint flags,
            IReadOnlyList<Message> parts)
        {
            BeforeNoBindReply?.Invoke(actor);
            if (!NoBindReplyAccepted)
                return false;

            NoBindReplies.Add(new CapturedActorReply(
                actor,
                sourceNodeRid,
                sourceSessionRid,
                requestId,
                flags,
                CopyParts(parts)));
            return true;
        }

        public bool ForwardActorBoundSessionPart(
            ZLinkBackendActorRef actor,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid,
            Message message,
            bool hasMore,
            SendFlags flags)
        {
            BeforeForwardActorBoundSessionPart?.Invoke();
            var result = !ForwardResults.TryDequeue(out var configured) || configured;
            lock (MessageFollowParts)
                MessageFollowParts.Add((hasMore, message.AsReadOnlySpan().ToArray()));
            if (!hasMore) FinalPartAttempted.TrySetResult();
            return result;
        }

        public void CloseActorBoundSession(
            ZLinkBackendActorRef actor,
            TimeSpan timeout,
            CancellationToken cancellationToken) { }

        private static IReadOnlyList<byte[]> CopyParts(IReadOnlyList<Message> parts)
        {
            return parts.Select(static part => part.AsReadOnlySpan().ToArray()).ToArray();
        }
    }

    private sealed class CapturingBackendAdapterFactory(
        CapturingSpotNode node) : IZLinkBackendAdapterFactory
    {
        public IZLinkBackendRuntimeContext CreateRuntimeContext() =>
            new CapturingBackendRuntimeContext(node);

        public IZLinkMonitoringBackendAdapter CreateMonitoringAdapter() => throw new NotSupportedException();
    }

    private sealed class CapturingBackendRuntimeContext(
        CapturingSpotNode node) : IZLinkBackendRuntimeContext
    {
        public void ConfigureCoreHwm(
            AutoHwmProfile profile,
            ulong memoryLimitBytes,
            ulong budgetBytes) { }

        public CoreHwmBudgetSnapshot GetCoreHwmBudgetSnapshot() =>
            throw new NotSupportedException();

        public void ResetCoreHwmBudgetMetrics() =>
            throw new NotSupportedException();

        public void ConfigureApplicationJobQueue(
            Zlink.Framework.Runtime.Dispatch.ZLinkApplicationJobQueue applicationJobQueue)
        {
        }

        public IDealerSocket CreateDealerSocket() =>
            throw new NotSupportedException();

        public IRouterSocket CreateRouterSocket() =>
            throw new NotSupportedException();

        public IPubSocket CreatePublisherSocket() =>
            throw new NotSupportedException();

        public ISubSocket CreateSubscriberSocket() =>
            throw new NotSupportedException();

        public IZLinkBackendSpotNode CreateSpotNode(string meshName) => node;

        public IZLinkBackendStreamSocket CreateStreamSocket(
            string standaloneMeshName,
            IZLinkBackendSpotNode? actorDispatchNode = null) =>
            throw new NotSupportedException();

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    private sealed class CapturingMessageFlowObserver : IDisposable
    {
        private readonly ActivityListener _listener;
        private readonly ConcurrentQueue<ObservedMessageFlow> _events = new();
        private readonly TaskCompletionSource<ObservedMessageFlow> _observed =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public CapturingMessageFlowObserver()
        {
            _listener = new ActivityListener
            {
                ShouldListenTo = source =>
                    source.Name == ZLinkTelemetry.ActivitySourceName,
                Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                    ActivitySamplingResult.AllDataAndRecorded,
                ActivityStopped = Capture
            };
            ActivitySource.AddActivityListener(_listener);
        }

        public IReadOnlyCollection<ObservedMessageFlow> Events => _events.ToArray();

        public async Task<ObservedMessageFlow> WaitAsync(TimeSpan timeout)
        {
            var deadline = DateTime.UtcNow + timeout;
            while (DateTime.UtcNow < deadline)
            {
                var observed = Events.FirstOrDefault(flow => flow.Phase == "sent");
                if (observed is not null) return observed;
                await Task.Delay(5);
            }

            throw new TimeoutException("Timed out waiting for message-flow phase 'sent'.");
        }

        public async Task<ObservedMessageFlow> WaitAsync(
            string outcome,
            TimeSpan timeout)
        {
            var deadline = DateTime.UtcNow + timeout;
            while (DateTime.UtcNow < deadline)
            {
                var observed = Events.FirstOrDefault(flow => flow.Outcome == outcome);
                if (observed is not null) return observed;
                await Task.Delay(5);
            }

            throw new TimeoutException($"Timed out waiting for message-flow outcome '{outcome}'.");
        }

        public void Dispose() => _listener.Dispose();

        private void Capture(Activity activity)
        {
            string? Tag(string name) =>
                activity.TagObjects.FirstOrDefault(tag => tag.Key == name).Value?.ToString();
            var phase = Normalize(Tag("phase") ?? activity.Events.FirstOrDefault().Name);
            var flow = new ObservedMessageFlow(
                phase,
                Tag("outcome")
                ?? (Tag("event_id") == "zlink.dispatch_error" ? "failed" : phase),
                Normalize(Tag("surface") ?? Tag("zlink.surface")),
                Normalize(Tag("message_kind") ?? Tag("zlink.kind")),
                Tag("packet_name") ?? Tag("zlink.packet.name"),
                Tag("channel_name") ?? Tag("zlink.channel.name"),
                Tag("correlation_id"),
                Tag("flow_id"),
                Tag("flow_origin"),
                Tag("actor_id") ?? Tag("zlink.actor.id"),
                Tag("source_rid") ?? Tag("zlink.source.rid"),
                Tag("target_rid") ?? Tag("zlink.peer.rid"),
                Tag("spot_id") ?? Tag("zlink.spot.id"),
                Normalize(Tag("reason") ?? Tag("zlink.reason")),
                Normalize(Tag("action") ?? Tag("zlink.action")));
            _events.Enqueue(flow);
            _observed.TrySetResult(flow);
        }

        private static string? Normalize(string? value) =>
            value switch
            {
                "SpotActor" => "actor",
                "StreamSession" => "stream",
                "ActorSend" => "send",
                "ActorRequest" => "request",
                "HandlerMissing" => "no_handler",
                "PayloadDecodeFailed" => "decode_error",
                "HandlerException" => "handler_exception",
                "InvalidFrame" => "invalid_frame",
                "ReplyPathMissing" => "reply_path_missing",
                "ReplyError" => "reply_error",
                "FailCaller" => "fail_caller",
                _ => value?.Replace('-', '_').ToLowerInvariant()
            };
    }

    private sealed record ObservedMessageFlow(
        string? Phase,
        string? Outcome,
        string? Surface,
        string? MessageKind,
        string? PacketName,
        string? ChannelName,
        string? CorrelationId,
        string? FlowId,
        string? FlowOrigin,
        string? ActorId,
        string? SourceRid,
        string? TargetRid,
        string? SpotId,
        string? Reason,
        string? Action);

    private sealed class ThrowingBackendAdapterFactory : IZLinkBackendAdapterFactory
    {
        public IZLinkBackendRuntimeContext CreateRuntimeContext() => throw new NotSupportedException();

        public IZLinkMonitoringBackendAdapter CreateMonitoringAdapter() => throw new NotSupportedException();
    }

    private sealed class ThrowingRuntimeErrorSink : IZLinkRuntimeFailureReporter
    {
        public void ReportHandlerException(Exception exception)
        {
            throw new InvalidOperationException("Runtime handler failed.", exception);
        }

        public void ReportUnhandledCallbackException(Exception exception)
        {
            throw new InvalidOperationException("Runtime callback failed.", exception);
        }

        public void ReportRuntimeTaskException(string name, Exception exception)
        {
            throw new InvalidOperationException($"Runtime task '{name}' failed.", exception);
        }
    }
}
