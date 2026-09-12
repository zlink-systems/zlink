using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class RouteMeshReceiveFlowTests
{
    [Fact]
    public async Task RouterTracksApplicationPressureUntilNodeClose()
    {
        await using var innerContext = Systems.Zlink.Zlink.CreateContext();
        var context = new CapturingRouterContext(innerContext);
        using var queue = new ZLinkApplicationJobQueue(new(
            ZLinkApplicationJobQueueProfile.Balanced, 1, 1, 1));
        var endpoint = $"inproc://route-mesh-receive-flow-{Guid.NewGuid():N}";
        var nodeRid = RoutingId.From("receive-flow-node");
        await using var node = new ZLinkManagedMeshNode(
            context,
            "receive-flow",
            applicationJobQueue: queue);
        node.SetRoutingId(nodeRid);
        node.SetBind(endpoint);
        node.Start();
        var router = context.GetRouter(nodeRid);

        var duplicateStates = new List<ReceiveFlowState>();
        using (queue.RegisterReceiveFlowSocket(router, duplicateStates.Add))
            Assert.Empty(duplicateStates);

        await using var dealer = context.CreateDealerSocket();
        dealer.SetRoutingId(RoutingId.From("receive-flow-peer"));
        using var monitor = dealer.MonitorOpen(
            SocketEvent.ConnectionReady
            | SocketEvent.SendFlowPaused
            | SocketEvent.SendFlowResumed);
        dealer.Connect(endpoint);
        await ReceiveFlowMonitor.ReceiveAsync(
            monitor,
            MonitorEventType.ConnectionReady);

        var lease = await queue.AcquireAsync(CancellationToken.None);
        await ReceiveFlowMonitor.ReceiveAsync(
            monitor,
            MonitorEventType.SendFlowPaused);
        lease.ReleaseForHandlerStart();
        await ReceiveFlowMonitor.ReceiveAsync(
            monitor,
            MonitorEventType.SendFlowResumed);

        await node.DisposeAsync();

        var afterCloseStates = new List<ReceiveFlowState>();
        using var afterCloseRegistration = queue.RegisterReceiveFlowSocket(
            router,
            afterCloseStates.Add);
        Assert.Equal([ReceiveFlowState.Running], afterCloseStates);
        using var afterCloseLease = await queue.AcquireAsync(CancellationToken.None);
        Assert.Equal(
            [ReceiveFlowState.Running, ReceiveFlowState.Paused],
            afterCloseStates);
        Assert.Equal(0UL, queue.GetPressureMetrics().FlowStateConfigFailures);
    }
}

internal static class ReceiveFlowMonitor
{
    internal static async Task<MonitorEvent> ReceiveAsync(
        ISocketMonitor monitor,
        MonitorEventType expected)
    {
        while (true)
        {
            var current = await Task.Factory.StartNew(
                    () => monitor.Recv(),
                    CancellationToken.None,
                    TaskCreationOptions.LongRunning,
                    TaskScheduler.Default)
                .WaitAsync(TimeSpan.FromSeconds(2));
            if (current?.Event == expected)
                return current;
        }
    }
}

internal sealed class CapturingRouterContext(IContext inner) : IContext
{
    private readonly List<IRouterSocket> _routers = [];

    public IContextOptions Options => inner.Options;

    public IRouterSocket CreateRouterSocket()
    {
        var router = inner.CreateRouterSocket();
        _routers.Add(router);
        return router;
    }

    internal IRouterSocket GetRouter(RoutingId routingId) =>
        Assert.Single(
            _routers,
            router => router.GetRoutingId() == routingId);

    public IPairSocket CreatePairSocket() => inner.CreatePairSocket();
    public IDealerSocket CreateDealerSocket() => inner.CreateDealerSocket();
    public IPubSocket CreatePubSocket() => inner.CreatePubSocket();
    public ISubSocket CreateSubSocket() => inner.CreateSubSocket();
    public IXPubSocket CreateXPubSocket() => inner.CreateXPubSocket();
    public IXSubSocket CreateXSubSocket() => inner.CreateXSubSocket();
    public IStreamSocket CreateStreamSocket() => inner.CreateStreamSocket();
    public void Shutdown() => inner.Shutdown();
    public void RecalculateAutoHwm() => inner.RecalculateAutoHwm();
    public CoreHwmBudgetSnapshot GetCoreHwmBudgetSnapshot() =>
        inner.GetCoreHwmBudgetSnapshot();
    public void ResetCoreHwmBudgetMetrics() =>
        inner.ResetCoreHwmBudgetMetrics();
    public void Dispose() => inner.Dispose();
    public ValueTask DisposeAsync() => inner.DisposeAsync();
}
