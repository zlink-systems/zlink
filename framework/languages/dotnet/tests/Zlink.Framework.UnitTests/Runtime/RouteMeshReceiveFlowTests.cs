using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class RouteMeshReceiveFlowTests
{
    [Fact]
    public async Task ClientDealerNeverReceivesHostPressureButServerRouterDoes()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            var channel = options.AddClientServerChannel("pressure");
            channel.Client();
            channel.Server().Listen().AddSendHandler<PressureHandler, PressureMessage>();
        });
        using var provider = services.BuildServiceProvider();
        var registration = provider.GetRequiredService<ZLinkFrameworkRegistration>();
        var inner = new ZLinkDotNetBackendAdapterFactory().CreateRuntimeContext();
        var context = new PressureContext(inner);
        using var errors = new ZLinkRuntimeErrorSink();
        await using var state = new ZLinkFrameworkComponentState(
            context,
            registration,
            provider,
            errors,
            new object(),
            new(ZLinkApplicationJobQueueProfile.Balanced, 1, 1, 1),
            new ZLinkListenerRecords()
        );
        var factory = new ZLinkChannelBundleFactory(registration);
        await using var client = await factory.CreateClientServerClientBundleAsync(
            state,
            "pressure",
            registration.Channels["pressure"]
        );
        using (
            var lease = await state.ApplicationJobQueue.AcquireAsync(
                CancellationToken.None,
                ZLinkApplicationJobOrigin.Remote
            )
        )
        {
            Assert.Equal(
                ZLinkApplicationJobQueuePressureState.Paused,
                state.ApplicationJobQueue.GetStatus().PressureState
            );
            Assert.Equal(0, context.Dealer!.FlowCalls);
        }
        Assert.Equal(0, context.Dealer!.FlowCalls);
        await using var server = await factory.CreateClientServerServerBundleAsync(
            state,
            "pressure",
            registration.Channels["pressure"]
        );
        var states = new List<ReceiveFlowState>();
        // A duplicate registration has no setter calls: the server already owns this target.
        await using var duplicate = state.ApplicationJobQueue.RegisterReceiveFlowSocket(
            context.Router!,
            states.Add
        );
        Assert.Empty(states);
        Assert.Equal(1, context.Router!.FlowCalls);
        var serverLease = await state.ApplicationJobQueue.AcquireAsync(
            CancellationToken.None,
            ZLinkApplicationJobOrigin.Remote
        );
        Assert.Equal(2, context.Router.FlowCalls);
        await serverLease.ReleaseForHandlerStartAsync();
        Assert.Equal(3, context.Router.FlowCalls);
        Assert.Equal(0, context.Dealer!.FlowCalls);
    }

    private sealed record PressureMessage;

    private sealed class PressureHandler : IZLinkSendHandler<PressureMessage>
    {
        public ValueTask HandleAsync(
            PressureMessage message,
            IZLinkMessageContext context,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

    [Fact]
    public async Task RouterTracksApplicationPressureUntilNodeClose()
    {
        await using var innerContext = Systems.Zlink.Zlink.CreateContext();
        var context = new CapturingRouterContext(innerContext);
        using var queue = new ZLinkApplicationJobQueue(
            new(ZLinkApplicationJobQueueProfile.Balanced, 1, 1, 1)
        );
        var endpoint = $"inproc://route-mesh-receive-flow-{Guid.NewGuid():N}";
        var nodeRid = RoutingId.From("receive-flow-node");
        await using var node = new ZLinkManagedMeshNode(
            context,
            "receive-flow",
            applicationJobQueue: queue
        );
        node.SetRoutingId(nodeRid);
        node.SetBind(endpoint);
        node.Start();
        var router = context.GetRouter(nodeRid);

        var duplicateStates = new List<ReceiveFlowState>();
        await using (queue.RegisterReceiveFlowSocket(router, duplicateStates.Add))
            Assert.Empty(duplicateStates);

        await using var dealer = context.CreateDealerSocket();
        dealer.SetRoutingId(RoutingId.From("receive-flow-peer"));
        using var monitor = dealer.MonitorOpen(
            SocketEvent.ConnectionReady | SocketEvent.SendFlowPaused | SocketEvent.SendFlowResumed
        );
        dealer.Connect(endpoint);
        await ReceiveFlowMonitor.ReceiveAsync(monitor, MonitorEventType.ConnectionReady);

        var lease = await queue.AcquireAsync(
            CancellationToken.None,
            ZLinkApplicationJobOrigin.Remote
        );
        await ReceiveFlowMonitor.ReceiveAsync(monitor, MonitorEventType.SendFlowPaused);
        await lease.ReleaseForHandlerStartAsync();
        await ReceiveFlowMonitor.ReceiveAsync(monitor, MonitorEventType.SendFlowResumed);

        await node.DisposeAsync();

        var afterCloseStates = new List<ReceiveFlowState>();
        await using var afterCloseRegistration = queue.RegisterReceiveFlowSocket(
            router,
            afterCloseStates.Add
        );
        Assert.Equal([ReceiveFlowState.Running], afterCloseStates);
        using var afterCloseLease = await queue.AcquireAsync(
            CancellationToken.None,
            ZLinkApplicationJobOrigin.Remote
        );
        Assert.Equal([ReceiveFlowState.Running, ReceiveFlowState.Paused], afterCloseStates);
        Assert.Equal(0UL, queue.GetPressureMetrics().FlowStateConfigFailures);
    }
}

internal sealed class PressureContext(IZLinkBackendRuntimeContext inner)
    : IZLinkBackendRuntimeContext
{
    internal PressureDealer? Dealer { get; private set; }
    internal PressureRouter? Router { get; private set; }

    public IDealerSocket CreateDealerSocket() =>
        Dealer = new PressureDealer(inner.CreateDealerSocket());

    public IRouterSocket CreateRouterSocket() =>
        Router = new PressureRouter(inner.CreateRouterSocket());

    public void ConfigureCoreHwm(
        AutoHwmProfile profile,
        ulong memoryLimitBytes,
        ulong budgetBytes
    ) => inner.ConfigureCoreHwm(profile, memoryLimitBytes, budgetBytes);

    public CoreHwmBudgetSnapshot GetCoreHwmBudgetSnapshot() => inner.GetCoreHwmBudgetSnapshot();

    public void ResetCoreHwmBudgetMetrics() => inner.ResetCoreHwmBudgetMetrics();

    public void ConfigureApplicationJobQueue(ZLinkApplicationJobQueue queue) =>
        inner.ConfigureApplicationJobQueue(queue);

    public IPubSocket CreatePublisherSocket() => inner.CreatePublisherSocket();

    public ISubSocket CreateSubscriberSocket() => inner.CreateSubscriberSocket();

    public IZLinkBackendSpotNode CreateSpotNode(string meshName) => inner.CreateSpotNode(meshName);

    public IZLinkBackendStreamSocket CreateStreamSocket(
        string meshName,
        IZLinkBackendSpotNode? actorDispatchNode = null
    ) => inner.CreateStreamSocket(meshName, actorDispatchNode);

    public ValueTask DisposeAsync() => inner.DisposeAsync();
}

internal abstract class PressureSocket<T>(T inner) : IReceivingMessageSocket, IConnectableSocket
    where T : IReceivingMessageSocket, IConnectableSocket
{
    internal int FlowCalls { get; private set; }
    public CommonSocketOptions Options => inner.Options;

    public void SetReceiveFlowState(ReceiveFlowState state)
    {
        FlowCalls++;
        inner.SetReceiveFlowState(state);
    }

    public void Bind(string address) => inner.Bind(address);

    public void Unbind(string address) => inner.Unbind(address);

    public void Connect(string address) => inner.Connect(address);

    public void Disconnect(string address) => inner.Disconnect(address);

    public void DisconnectRid(RoutingId rid) => inner.DisconnectRid(rid);

    public bool Recv(Received result, RecvFlags flags = RecvFlags.None) =>
        inner.Recv(result, flags);

    public ISocketMonitor MonitorOpen(SocketEvent events = SocketEvent.All) =>
        inner.MonitorOpen(events);

    public ISocketMonitor MonitorOpen(SocketEvent events, ulong bytes) =>
        inner.MonitorOpen(events, bytes);

    public void SetTlsServer(string cert, string key, bool requireClientCert = false) =>
        inner.SetTlsServer(cert, key, requireClientCert);

    public void SetTlsClient(string ca, string host, bool trustSystem = false) =>
        inner.SetTlsClient(ca, host, trustSystem);

    public void Close() => inner.Close();

    public void Dispose() => inner.Dispose();

    public ValueTask DisposeAsync() => inner.DisposeAsync();
}

internal sealed class PressureDealer(IDealerSocket inner)
    : PressureSocket<IDealerSocket>(inner),
        IDealerSocket
{
    public new DealerSocketOptions Options => inner.Options;

    public void SetRoutingId(RoutingId rid) => inner.SetRoutingId(rid);

    public RoutingId GetRoutingId() => inner.GetRoutingId();

    public SendOperation Send() => inner.Send();

    public RequestOperation Request() => inner.Request();
}

internal sealed class PressureRouter(IRouterSocket inner)
    : PressureSocket<IRouterSocket>(inner),
        IRouterSocket
{
    public new RouterSocketOptions Options => inner.Options;

    public void SetRoutingId(RoutingId rid) => inner.SetRoutingId(rid);

    public RoutingId GetRoutingId() => inner.GetRoutingId();

    public SendOperation Send(RoutingId rid) => inner.Send(rid);

    public RequestOperation Request(RoutingId rid) => inner.Request(rid);

    public ReplyOperation Reply(RoutingId rid, ReplyToken token) => inner.Reply(rid, token);

    public IReadOnlyList<RouterRoute> RoutesSnapshot() => inner.RoutesSnapshot();
}

internal static class ReceiveFlowMonitor
{
    internal static async Task<MonitorEvent> ReceiveAsync(
        ISocketMonitor monitor,
        MonitorEventType expected
    )
    {
        while (true)
        {
            var current = await Task
                .Factory.StartNew(
                    () => monitor.Recv(),
                    CancellationToken.None,
                    TaskCreationOptions.LongRunning,
                    TaskScheduler.Default
                )
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
        Assert.Single(_routers, router => router.GetRoutingId() == routingId);

    public IPairSocket CreatePairSocket() => inner.CreatePairSocket();

    public IDealerSocket CreateDealerSocket() => inner.CreateDealerSocket();

    public IPubSocket CreatePubSocket() => inner.CreatePubSocket();

    public ISubSocket CreateSubSocket() => inner.CreateSubSocket();

    public IXPubSocket CreateXPubSocket() => inner.CreateXPubSocket();

    public IXSubSocket CreateXSubSocket() => inner.CreateXSubSocket();

    public IStreamSocket CreateStreamSocket() => inner.CreateStreamSocket();

    public void Shutdown() => inner.Shutdown();

    public void RecalculateAutoHwm() => inner.RecalculateAutoHwm();

    public CoreHwmBudgetSnapshot GetCoreHwmBudgetSnapshot() => inner.GetCoreHwmBudgetSnapshot();

    public void ResetCoreHwmBudgetMetrics() => inner.ResetCoreHwmBudgetMetrics();

    public void Dispose() => inner.Dispose();

    public ValueTask DisposeAsync() => inner.DisposeAsync();
}
