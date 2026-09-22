using System.Collections.Concurrent;
using System.Diagnostics;
using System.Reflection;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet.Wrappers;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshChannelSelectionHotPathTests
{
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
        TimeSpan timeout,
        CancellationToken cancellationToken
    );

    [Fact]
    public async Task ReadyRequestSelectionAllocatesNoDeadlineBeyondSynchronousSelection()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "selection");
        var peer = AddReadyPeer(node);
        var select = Method<SelectTarget>(node, "TrySelectChannelTarget");
        var wait = Method<WaitForTarget>(node, "WaitForChannelTargetAsync");
        using var cancellation = new CancellationTokenSource();

        // Warm both existing selector paths, including JIT and AsyncLocal state.
        for (var index = 0; index < 256; index++)
        {
            select("worker");
            wait("worker", TimeSpan.FromMinutes(1), cancellation.Token).GetAwaiter().GetResult();
        }

        const int iterations = 1024;
        var start = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
            select("worker");
        var synchronousBytes = GC.GetAllocatedBytesForCurrentThread() - start;
        start = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
            wait("worker", TimeSpan.FromMinutes(1), cancellation.Token).GetAwaiter().GetResult();
        var requestBytes = GC.GetAllocatedBytesForCurrentThread() - start;

        // A linked CTS plus its deadline timer cannot fit in this allowance.
        // The reference path performs the same selector and peer lookup.
        Assert.True(
            requestBytes <= synchronousBytes + iterations * 32L,
            $"Ready request selection allocated {requestBytes} bytes; "
                + $"synchronous selection allocated {synchronousBytes} bytes."
        );

        cancellation.Cancel();
        var selected = await wait("worker", TimeSpan.FromMinutes(1), cancellation.Token);
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
        var wait = Method<WaitForTarget>(node, "WaitForChannelTargetAsync");
        var selected = await wait("worker", TimeSpan.FromSeconds(1), CancellationToken.None);

        peer.PhysicalRoutingId = RoutingId.From("replacement-physical");

        Assert.Equal(peer.RoutingId, selected.TargetRid);
        Assert.Equal(originalPhysical, selected.PhysicalRid);
        var next = await wait("worker", TimeSpan.FromSeconds(1), CancellationToken.None);
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
            Field<ZLinkMeshChannelSelection>(node, "_channelSelection")
                .Rebuild(
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
            var start = GC.GetAllocatedBytesForCurrentThread();
            node.Publish(
                "publisher",
                "observer",
                "off",
                [payload],
                SendFlags.None,
                ReadOnlyMemory<byte>.Empty
            );
            var offBytes = GC.GetAllocatedBytesForCurrentThread() - start;

            registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            start = GC.GetAllocatedBytesForCurrentThread();
            node.Publish(
                "publisher",
                "observer",
                "normal",
                [payload],
                SendFlags.None,
                ReadOnlyMemory<byte>.Empty
            );
            var normalBytes = GC.GetAllocatedBytesForCurrentThread() - start;
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
        var wait = Method<WaitForTarget>(node, "WaitForChannelTargetAsync");
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            wait("worker", TimeSpan.FromSeconds(1), cancellation.Token).AsTask()
        );

        var timeout = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            wait("worker", TimeSpan.FromTicks(1), CancellationToken.None)
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(3))
        );
        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, timeout.Kind);
    }

    private static ZLinkMeshPeer AddReadyPeer(ZLinkManagedMeshNode node)
    {
        var peer = new ZLinkMeshPeer(
            1,
            "inproc://selection-peer",
            null,
            "",
            ZLinkServiceConnectionDirection.Outbound
        )
        {
            RoutingId = RoutingId.From("logical-peer"),
            PhysicalRoutingId = RoutingId.From("physical-peer"),
            Admitted = true,
        };
        Field<Dictionary<RoutingId, ZLinkMeshPeer>>(node, "_peersByRid").Add(peer.RoutingId, peer);
        Field<ZLinkMeshChannelSelection>(node, "_channelSelection")
            .Rebuild(["worker"], _ => [new ZLinkMeshChannelTarget(peer.RoutingId, 1)]);
        return peer;
    }

    private static ZLinkMeshPeer AddPeer(ZLinkManagedMeshNode node, string routingId, bool admitted)
    {
        var peer = new ZLinkMeshPeer(
            routingId == "stale-peer" ? 1UL : 2UL,
            $"inproc://{routingId}",
            null,
            "",
            ZLinkServiceConnectionDirection.Outbound
        )
        {
            RoutingId = RoutingId.From(routingId),
            PhysicalRoutingId = RoutingId.From($"physical-{routingId}"),
            Admitted = admitted,
        };
        Field<Dictionary<RoutingId, ZLinkMeshPeer>>(node, "_peersByRid").Add(peer.RoutingId, peer);
        return peer;
    }

    private static T Method<T>(ZLinkManagedMeshNode node, string name)
        where T : Delegate =>
        typeof(ZLinkManagedMeshNode)
            .GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic)!
            .CreateDelegate<T>(node);

    private static T Field<T>(ZLinkManagedMeshNode node, string name) =>
        (T)
            typeof(ZLinkManagedMeshNode)
                .GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!
                .GetValue(node)!;

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
