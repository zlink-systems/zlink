using System.Collections.Concurrent;
using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshChannelSelectionHotPathTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task AdmissionAndRouteLossRefreshEveryRouterClassification(bool localIsClient)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "classification-transitions");
        node.SetObjectRole(
            localIsClient ? ZLinkMeshNodeObjectRole.Client : ZLinkMeshNodeObjectRole.Server
        );
        var rid = RoutingId.From("transition-peer");
        const string endpoint = "inproc://classification-transitions";
        node.ConnectPeer(endpoint, rid);
        var admission = new ZLinkServiceWireCodec.AdmissionRecord(
            "classification-transitions",
            ZLinkServiceSecurityIdentity.Plaintext,
            endpoint,
            1,
            1,
            new Dictionary<string, uint>(),
            1,
            1,
            (byte)ZLinkMeshNodeObjectRole.Server,
            100,
            0,
            0,
            0,
            0,
            new Dictionary<byte, byte[]>(),
            []
        );
        Action<
            RoutingId,
            ulong,
            ServiceWireConstants.Command,
            ZLinkServiceWireCodec.AdmissionRecord,
            SendOperation?
        > process = node.ProcessAdmissionCore;
        process(rid, 1UL, ServiceWireConstants.Command.Hello, admission, null);
        // Transport liveness §5: local registration does not establish send readiness.
        Assert.Equal(
            ZLinkRouteMeshTargetClassification.Unknown,
            node.ClassifyLiveRouterTarget(rid)
        );
        AssertDirectPeerPublished(node, rid);
        process(rid, 1UL, ServiceWireConstants.Command.Admit, admission, null);
        Assert.Equal(
            ZLinkRouteMeshTargetClassification.ReadyEligible,
            node.ClassifyLiveRouterTarget(rid)
        );
        AssertDirectPeerPublished(node, rid);
        var draining = admission with { DescriptorRevision = 2, RuntimeState = 2 };
        process(rid, 1UL, ServiceWireConstants.Command.Update, draining, null);
        Assert.Equal(
            ZLinkRouteMeshTargetClassification.Unknown,
            node.ClassifyLiveRouterTarget(rid)
        );
        AssertDirectPeerPublished(node, rid);
        node.EndRouteAdmissionUnderLock(new RouterRoute(rid, 1));
        Assert.Equal(
            ZLinkRouteMeshTargetClassification.Unknown,
            node.ClassifyLiveRouterTarget(rid)
        );
        AssertDirectPeerPublished(node, rid);
        var client = admission with
        {
            LifecycleGeneration = 2,
            ObjectRole = (byte)ZLinkMeshNodeObjectRole.Client,
        };
        process(rid, 2UL, ServiceWireConstants.Command.Hello, client, null);
        Assert.Equal(
            ZLinkRouteMeshTargetClassification.ObjectClientTarget,
            node.ClassifyLiveRouterTarget(rid)
        );
        AssertDirectPeerPublished(node, rid);
        process(rid, 2UL, ServiceWireConstants.Command.Admit, client, null);
        Assert.Equal(
            ZLinkRouteMeshTargetClassification.ObjectClientTarget,
            node.ClassifyLiveRouterTarget(rid)
        );
        AssertDirectPeerPublished(node, rid);
        var invalidRoleUpdate = client with
        {
            DescriptorRevision = client.DescriptorRevision + 1,
            ObjectRole = (byte)ZLinkMeshNodeObjectRole.Server,
        };
        process(rid, 2UL, ServiceWireConstants.Command.Update, invalidRoleUpdate, null);
        Assert.Equal(
            ZLinkRouteMeshTargetClassification.Unknown,
            node.ClassifyLiveRouterTarget(rid)
        );
        AssertDirectPeerPublished(node, rid);
        var replacement = admission with { LifecycleGeneration = 3 };
        process(rid, 3UL, ServiceWireConstants.Command.Admit, replacement, null);
        Assert.Equal(
            ZLinkRouteMeshTargetClassification.ReadyEligible,
            node.ClassifyLiveRouterTarget(rid)
        );
        AssertDirectPeerPublished(node, rid);
        node.DisconnectPeer(rid);
        Assert.Equal(
            ZLinkRouteMeshTargetClassification.Unknown,
            node.ClassifyLiveRouterTarget(rid)
        );
        AssertDirectPeerPublished(node, rid);
    }

    [Fact]
    public async Task RepeatedSendTargetClassificationDoesNotEnterTheNodeStateLane()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "classification-turn");
        var peer = AddReadyPeer(node);
        peer.State = MeshPeerState.Admitted;
        node.RebuildChannelSelectionPlansUnderLock();
        var lane = node._lane;
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var held = lane.RunAsync(async () =>
        {
            entered.SetResult();
            await release.Task;
        });
        await entered.Task;
        var classifications = Task.Run(() =>
        {
            for (var index = 0; index < 1024; index++)
                Assert.Equal(
                    ZLinkRouteMeshTargetClassification.ReadyEligible,
                    node.ClassifyLiveRouterTarget(peer.RoutingId)
                );
        });
        try
        {
            // A node state turn cannot run while the existing turn is held.
            // All classifications must finish without requesting such a turn.
            await classifications.WaitAsync(TimeSpan.FromSeconds(3));
        }
        finally
        {
            release.SetResult();
            await held;
            await classifications;
        }
    }

    [Fact]
    public async Task LiveTargetClassificationUsesTheMonitoringAdmissionState()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "classification");
        var peer = AddReadyPeer(node);
        Assert.Equal(
            ZLinkRouteMeshTargetClassification.Unknown,
            node.ClassifyLiveRouterTarget(peer.RoutingId)
        );
        peer.State = MeshPeerState.Admitted;
        node.RebuildChannelSelectionPlansUnderLock();
        Assert.Equal(
            ZLinkRouteMeshTargetClassification.ReadyEligible,
            node.ClassifyLiveRouterTarget(peer.RoutingId)
        );
        peer.State = MeshPeerState.Configured;
        node.RebuildChannelSelectionPlansUnderLock();
        Assert.Equal(
            ZLinkRouteMeshTargetClassification.Unknown,
            node.ClassifyLiveRouterTarget(peer.RoutingId)
        );
    }

    private delegate (
        bool Selected,
        RoutingId TargetRid,
        RoutingId PhysicalRid,
        SubmitResult Failure,
        string FailureReason,
        bool Wait,
        Task Changed
    ) SelectTarget(string channel);

    private delegate ValueTask<(RoutingId TargetRid, RoutingId PhysicalRid)> WaitForTarget(
        string channel,
        Task terminal,
        CancellationToken cancellationToken
    );

    [Fact]
    public async Task ReadyRequestSelectionAllocatesNoDeadlineBeyondSynchronousSelection()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "selection");
        var peer = AddReadyPeer(node);
        SelectTarget select = node.TrySelectChannelTarget;
        WaitForTarget wait = node.WaitForChannelTargetAsync;
        var terminal = new TaskCompletionSource().Task;
        using var cancellation = new CancellationTokenSource();

        // Warm both existing selector paths, including JIT and AsyncLocal state.
        for (var index = 0; index < 256; index++)
        {
            select("worker");
            wait("worker", terminal, cancellation.Token).GetAwaiter().GetResult();
        }

        const int iterations = 1024;
        var synchronousBytes = PerCallAllocation.MinWindow(() => select("worker"), iterations);
        var requestBytes = PerCallAllocation.MinWindow(
            () => wait("worker", terminal, cancellation.Token).GetAwaiter().GetResult(),
            iterations
        );

        // A linked CTS plus its deadline timer cannot fit in this allowance.
        // The reference path performs the same selector and peer lookup.
        Assert.True(
            requestBytes <= synchronousBytes + iterations * 32L,
            $"Ready request selection allocated {requestBytes} bytes; "
                + $"synchronous selection allocated {synchronousBytes} bytes."
        );

        cancellation.Cancel();
        var selected = await wait("worker", terminal, cancellation.Token);
        Assert.Equal(peer.RoutingId, selected.TargetRid);
        Assert.Equal(peer.PhysicalRoutingId, selected.PhysicalRid);
    }

    [Fact]
    public async Task SelectionTransfersTheChosenPhysicalIdentityWithoutRetainingMutablePeerState()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "selection");
        var peer = AddReadyPeer(node);
        var originalPhysical = peer.PhysicalRoutingId;
        WaitForTarget wait = node.WaitForChannelTargetAsync;
        var terminal = new TaskCompletionSource().Task;
        var selected = await wait("worker", terminal, CancellationToken.None);

        peer.PhysicalRoutingId = RoutingId.From("replacement-physical");
        node._channelSelection.PrepareRouterTargets(node._peersByRid);

        Assert.Equal(peer.RoutingId, selected.TargetRid);
        Assert.Equal(originalPhysical, selected.PhysicalRid);
        var next = await wait("worker", terminal, CancellationToken.None);
        Assert.Equal(peer.PhysicalRoutingId, next.PhysicalRid);
    }

    [Fact]
    public async Task LogicalMulticastUnadmittedTargetUsesProductionReporterAndOffAttachesNoObserver()
    {
        var activities = new ConcurrentQueue<Activity>();
        using var listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllDataAndRecorded,
            ActivityStopped = activities.Enqueue,
        };
        ActivitySource.AddActivityListener(listener);

        var scheduler = new GatedTaskScheduler();
        var backend = new GatedManagedBackendAdapterFactory(scheduler);
        await using var services = new ServiceCollection().BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration
        {
            ImplicitHandlerAutoRegistrationEnabled = false,
        };
        registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Off);
        registration.SpotNodes["selection"] = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "selection",
            RoutingId = RoutingId.From("selection-source"),
            Router = new ZLinkSpotRouterCapabilityRegistration
            {
                BindEndpoint = $"inproc://selection-{Guid.NewGuid():N}",
            },
        };
        var runtime = new ZLinkFrameworkRuntime(
            services,
            backend,
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration
            )
        );

        await runtime.StartAsync(CancellationToken.None);
        try
        {
            var node = Assert
                .IsType<ZLinkBackendSpotNodeWrapper>(runtime.GetSpotNodeRuntime("selection").Node)
                .NativeNode;
            AddPeer(node, "stale-peer", admitted: false);
            AddPeer(node, "observer-peer", admitted: true);
            node._channelSelection.Rebuild(
                ["worker", "observer"],
                channel =>
                    channel == "worker"
                        ? [new ZLinkMeshChannelTarget(RoutingId.From("stale-peer"), 1)]
                        : [new ZLinkMeshChannelTarget(RoutingId.From("observer-peer"), 1)]
            );

            using var payload = Message.From("payload");
            node.Publish(
                "publisher",
                "worker",
                "orders",
                [payload],
                SendFlags.None,
                ReadOnlyMemory<byte>.Empty
            );
            Assert.Empty(activities);

            node.Publish(
                "publisher",
                "observer",
                "warmup",
                [payload],
                SendFlags.None,
                ReadOnlyMemory<byte>.Empty
            );
            var offBytes = PerCallAllocation.MinWindow(
                () =>
                    node.Publish(
                        "publisher",
                        "observer",
                        "off",
                        [payload],
                        SendFlags.None,
                        ReadOnlyMemory<byte>.Empty
                    ),
                64
            );

            registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            var normalBytes = PerCallAllocation.MinWindow(
                () =>
                    node.Publish(
                        "publisher",
                        "observer",
                        "normal",
                        [payload],
                        SendFlags.None,
                        ReadOnlyMemory<byte>.Empty
                    ),
                64
            );
            Assert.True(
                normalBytes > offBytes,
                $"Diagnostics Off allocated {offBytes} bytes; Normal allocated {normalBytes} bytes."
            );

            node.Publish(
                "publisher",
                "worker",
                "orders",
                [payload],
                SendFlags.None,
                ReadOnlyMemory<byte>.Empty
            );
            Assert.True(
                SpinWait.SpinUntil(
                    () =>
                        activities.Any(candidate =>
                            candidate.OperationName == "zlink.dispatch_error"
                        ),
                    TimeSpan.FromSeconds(5)
                )
            );
            var activity = Assert.Single(
                activities.Where(candidate => candidate.OperationName == "zlink.dispatch_error")
            );
            Assert.Equal("zlink.dispatch_error", activity.GetTagItem("event_id"));
            Assert.Equal("spot", activity.GetTagItem("surface"));
            Assert.Equal("send", activity.GetTagItem("message_kind"));
            Assert.Equal("failed", activity.GetTagItem("outcome"));
            Assert.Equal("drop", activity.GetTagItem("action"));
            Assert.Equal("stale_target", activity.GetTagItem("reason"));
            Assert.Equal("stale-peer", activity.GetTagItem("target_rid"));
            Assert.Equal("orders", activity.GetTagItem("topic"));
            Assert.Equal("worker", activity.GetTagItem("channel_name"));
            Assert.Equal("selection", activity.GetTagItem("mesh_name"));
            Assert.Null(activity.GetTagItem("phase"));
            Assert.Null(activity.GetTagItem("source_rid"));
            Assert.Null(activity.GetTagItem("packet_name"));
            Assert.Null(activity.GetTagItem("error_type"));
            Assert.Null(activity.GetTagItem("error_message"));
        }
        finally
        {
            scheduler.Release();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task UnresolvedFirstAdmissionPreservesCancellationAndDeadlineErrors()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "selection");
        node.ConnectPeer("inproc://selection-not-started", RoutingId.From("pending-peer"));
        WaitForTarget wait = node.WaitForChannelTargetAsync;
        var terminal = new TaskCompletionSource().Task;
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            wait("worker", terminal, cancellation.Token).AsTask()
        );

        var timeout = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            // §10.1: the entry owner supplies expiry; the selector only propagates it.
            wait(
                    "worker",
                    Task.FromException(
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DeadlineExceeded,
                            "Entry deadline expired."
                        )
                    ),
                    CancellationToken.None
                )
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(3))
        );
        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, timeout.Kind);
    }

    [Fact]
    public async Task RepeatedDirectPeerReadsDoNotEnterTheNodeStateLane()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "direct-peer-turn");
        var peer = AddReadyPeer(node);
        Func<RoutingId, ulong, ZLinkMeshDirectPeer> require = node.RequireDirectPeer;
        var lane = node._lane;
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var held = lane.RunAsync(async () =>
        {
            entered.SetResult();
            await release.Task;
        });
        await entered.Task;
        var reads = Task.Run(() =>
        {
            for (var index = 0; index < 1024; index++)
                Assert.Equal(peer.PhysicalRoutingId, require(peer.RoutingId, 0).PhysicalRoutingId);
        });
        try
        {
            // A node state turn cannot run while the existing turn is held.
            await reads.WaitAsync(TimeSpan.FromSeconds(3));
        }
        finally
        {
            release.SetResult();
            await held;
            await reads;
        }
    }

    // Command 40 and its state chunks leave on one ordered connection (wire
    // protocol, relocation direct chunk transfer). A published direct-peer view
    // whose physical route differs from the peer object the chunks use is a
    // route replacement: the attempt submits nothing and replays (actor model
    // §8.1) until the deadline. The node is not started, so a command 40 sent
    // on the view's route would fail with ObjectDisposedException instead.
    [Fact]
    public async Task Relocation_prepare_does_not_submit_when_the_view_route_differs_from_the_chunk_route()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = new ZLinkManagedMeshNode(context, "mesh");
        var targetRid = RoutingId.From("prepare-route-target");
        var peer = new ZLinkMeshPeer(1)
        {
            RoutingId = targetRid,
            PhysicalRoutingId = RoutingId.From("prepare-route-old"),
            Admitted = true,
            State = MeshPeerState.Admitted,
            RouteGeneration = 7,
            LifecycleGeneration = 1,
        };
        var peers = source._peersByRid;
        await source._lane.RunAsync(() =>
        {
            peers.Add(targetRid, peer);
            source._channelSelection.PrepareRouterTargets(peers);
            source._selectedRoutes.Apply([new RouterRoute(targetRid, 7)]);
            // The peer object moved to a new physical route after the view
            // was published.
            peer.PhysicalRoutingId = RoutingId.From("prepare-route-new");
        });
        var payload = ZLinkRelocationTransferPayload.Create(
            new ZLinkRelocationEnvelope(
                Guid.NewGuid(),
                1,
                new byte[32],
                [
                    new ZLinkRelocationParticipantEnvelope(
                        new ZLinkAuthorityKey("actor:mesh:actor"),
                        ZLinkPlacementObjectKind.Actor,
                        1,
                        1,
                        new byte[] { 1 },
                        [],
                        []
                    ),
                ]
            ),
            1024
        );
        var sourceRid = RoutingId.From("prepare-route-source");
        var prepare = new ZLinkServiceWireCodec.RelocationPrepareRecord(
            new(1, 1),
            1,
            new("source-owner", 1, sourceRid, 1, "v-source"),
            new(targetRid, 1, "target-owner", 1),
            1,
            new(1, "actor", "actor", 1, 1),
            sourceRid,
            1,
            (ulong)payload.TotalLength,
            (uint)payload.ChunkCount,
            payload.ChecksumCrc32c,
            1,
            checked((ulong)DateTimeOffset.UtcNow.AddMinutes(1).ToUnixTimeMilliseconds())
        );

        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await source.PrepareCanonicalRelocationAsync(
                targetRid,
                prepare,
                payload,
                TimeSpan.FromMilliseconds(100),
                CancellationToken.None
            )
        );

        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
    }

    // Every peer change point publishes the same direct-send facts the peer
    // table holds, so a send that reads the view sees what a node turn would.
    private static void AssertDirectPeerPublished(ZLinkManagedMeshNode node, RoutingId rid)
    {
        var published = node._channelSelection.DirectPeer(rid);
        if (!node._peersByRid.TryGetValue(rid, out var peer))
        {
            Assert.Null(published);
            return;
        }
        Assert.NotNull(published);
        Assert.Equal(peer.Admitted, published.Admitted);
        Assert.Equal(peer.PhysicalRoutingId, published.PhysicalRoutingId);
        Assert.Equal(peer.LifecycleGeneration, published.LifecycleGeneration);
        Assert.Same(peer.Liveness, published.Liveness);
    }

    private static ZLinkMeshPeer AddReadyPeer(ZLinkManagedMeshNode node)
    {
        var peer = new ZLinkMeshPeer(1)
        {
            RoutingId = RoutingId.From("logical-peer"),
            PhysicalRoutingId = RoutingId.From("physical-peer"),
            Admitted = true,
        };
        var peers = node._peersByRid;
        peers.Add(peer.RoutingId, peer);
        var selection = node._channelSelection;
        // The node publishes peer facts at every peer change; this fixture
        // changes the peer table directly, so it publishes them the same way.
        selection.PrepareRouterTargets(peers);
        selection.Rebuild(["worker"], _ => [new ZLinkMeshChannelTarget(peer.RoutingId, 1)]);
        return peer;
    }

    private static ZLinkMeshPeer AddPeer(ZLinkManagedMeshNode node, string routingId, bool admitted)
    {
        var peer = new ZLinkMeshPeer(routingId == "stale-peer" ? 1UL : 2UL)
        {
            RoutingId = RoutingId.From(routingId),
            PhysicalRoutingId = RoutingId.From($"physical-{routingId}"),
            Admitted = admitted,
        };
        node._peersByRid.Add(peer.RoutingId, peer);
        return peer;
    }

    private sealed class GatedManagedBackendAdapterFactory(GatedTaskScheduler scheduler)
        : IZLinkBackendAdapterFactory
    {
        public IZLinkBackendRuntimeContext CreateRuntimeContext() => new Context(scheduler);

        public IZLinkMonitoringBackendAdapter CreateMonitoringAdapter() =>
            throw new NotSupportedException();

        private sealed class Context(GatedTaskScheduler scheduler) : IZLinkBackendRuntimeContext
        {
            private readonly IContext _nativeContext = Systems.Zlink.Zlink.CreateContext();

            public void ConfigureCoreHwm(
                AutoHwmProfile profile,
                ulong memoryLimitBytes,
                ulong budgetBytes
            ) { }

            public CoreHwmBudgetSnapshot GetCoreHwmBudgetSnapshot() =>
                throw new NotSupportedException();

            public void ResetCoreHwmBudgetMetrics() => throw new NotSupportedException();

            public void ConfigureApplicationJobQueue(
                ZLinkApplicationJobQueue applicationJobQueue
            ) { }

            public IDealerSocket CreateDealerSocket() => throw new NotSupportedException();

            public IRouterSocket CreateRouterSocket() => throw new NotSupportedException();

            public IPubSocket CreatePublisherSocket() => throw new NotSupportedException();

            public ISubSocket CreateSubscriberSocket() => throw new NotSupportedException();

            public IZLinkBackendSpotNode CreateSpotNode(string meshName) =>
                new ZLinkBackendSpotNodeWrapper(
                    new ZLinkManagedMeshNode(
                        _nativeContext,
                        meshName,
                        routedSubmitScheduler: scheduler
                    )
                );

            public IZLinkBackendStreamSocket CreateStreamSocket(
                string standaloneMeshName,
                IZLinkBackendSpotNode? actorDispatchNode = null
            ) => throw new NotSupportedException();

            public ValueTask DisposeAsync() => _nativeContext.DisposeAsync();
        }
    }
}
