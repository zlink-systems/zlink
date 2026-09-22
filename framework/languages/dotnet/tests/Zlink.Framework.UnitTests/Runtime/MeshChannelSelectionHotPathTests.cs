using System.Diagnostics;
using System.Reflection;
using Zlink.Framework.Runtime.Diagnostics;
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
    public async Task LogicalMulticastNotReadyTargetReportsStaleTargetWithoutChangingTerminal()
    {
        var activities = new List<Activity>();
        using var listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllDataAndRecorded,
            ActivityStopped = activities.Add,
        };
        ActivitySource.AddActivityListener(listener);

        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Errors);
        options.Diagnostics.SetSampleRate(0);
        var tracer = new ZLinkMessageFlowTracer(options);

        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "selection");
        var peer = AddReadyPeer(node);
        peer.Admitted = false;
        node.SetLogicalMulticastFailureObserver(
            (channel, topic, targetRid, reason, exception) =>
                tracer.TraceDispatchError(
                    new ZLinkDispatchFailure(
                        ZLinkDispatchErrorSurface.SpotRoute,
                        ZLinkDispatchMessageKind.Send,
                        reason,
                        ZLinkDispatchErrorAction.Drop,
                        PacketName: null,
                        ChannelName: channel,
                        Topic: topic,
                        MeshName: "selection",
                        TargetRid: targetRid.ToString(),
                        Exception: exception
                    )
                )
        );

        await using var pool = new ZLinkWorkerPool(0, 1, TimeSpan.FromSeconds(30));
        var released = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var payload = Message.From("payload");
        var result = await ZLinkLogicalMulticastSubmitter.SubmitAsync(
            pool,
            () =>
                node.Publish(
                    "publisher",
                    "worker",
                    "topic",
                    [payload],
                    SendFlags.None,
                    ReadOnlyMemory<byte>.Empty
                ),
            CancellationToken.None,
            CancellationToken.None,
            TimeSpan.FromSeconds(1),
            () => released.TrySetResult(),
            new NoopRuntimeFailureReporter()
        );

        Assert.Equal(SubmitResult.Ok, result);
        await released.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var activity = Assert.Single(
            activities.Where(candidate => candidate.OperationName == "zlink.dispatch_error")
        );
        Assert.Equal("zlink.dispatch_error", activity.GetTagItem("event_id"));
        Assert.Null(activity.GetTagItem("phase"));
        Assert.Equal("spot", activity.GetTagItem("surface"));
        Assert.Equal("send", activity.GetTagItem("message_kind"));
        Assert.Equal("failed", activity.GetTagItem("outcome"));
        Assert.Equal("drop", activity.GetTagItem("action"));
        Assert.Equal("stale_target", activity.GetTagItem("reason"));
        Assert.Equal("selection", activity.GetTagItem("mesh_name"));
        Assert.Equal("worker", activity.GetTagItem("channel_name"));
        Assert.Equal("topic", activity.GetTagItem("topic"));
        Assert.Equal("logical-peer", activity.GetTagItem("target_rid"));
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

    private sealed class NoopRuntimeFailureReporter : IZLinkRuntimeFailureReporter
    {
        public void ReportHandlerException(Exception exception) { }

        public void ReportUnhandledCallbackException(Exception exception) { }

        public void ReportRuntimeTaskException(string taskName, Exception exception) { }
    }
}
