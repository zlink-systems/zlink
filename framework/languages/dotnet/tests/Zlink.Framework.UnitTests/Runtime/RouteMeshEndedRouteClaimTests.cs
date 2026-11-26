using System.Diagnostics;
using System.Threading.Channels;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Xunit.Abstractions;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests;

public sealed class RouteMeshEndedRouteClaimTests(ITestOutputHelper output)
{
    private const string MeshName = "ended-route";
    private const string ChannelName = "orders";
    private const ulong Lifecycle = 17;
    private static readonly TimeSpan ObservationTimeout = TimeSpan.FromSeconds(5);

    [Fact]
    public async Task CoreBufferedChannelSendReachesHandlerOnceAfterSnapshotEndsAdmission()
    {
        await using var fixture = new Fixture();
        await using var first = fixture.Connect();
        var generation = await fixture.AdmitAsync(first);
        SendChannel(first, "current");
        Assert.Equal("current", await fixture.NextDeliveryAsync());
        await fixture.Router.CurrentDataDrained.WaitAsync(ObservationTimeout);

        var (blocked, receipt) = fixture.Router.HoldUntilRouteEnds();
        SendChannel(first, "ended");
        await blocked.WaitAsync(ObservationTimeout);
        first.Close();

        var observed = await receipt.WaitAsync(ObservationTimeout);
        output.WriteLine(
            $"Core receipt generation={observed.Receipt}, applied selected generation={observed.Selected}"
        );
        Assert.Equal((generation, 0UL), observed);

        await using var replacement = fixture.Connect();
        Assert.NotEqual(generation, await fixture.AdmitAsync(replacement));
        SendChannel(replacement, "replacement");
        Assert.Equal(
            new[] { "ended", "replacement" },
            await fixture.DeliveriesThroughReplacementAsync()
        );
    }

    [Fact]
    public async Task ReplacementChannelSendRequiresRemoteAdmit()
    {
        await using var fixture = new Fixture();
        var generation = await fixture.AdmitAndCloseAsync();

        await using var replacement = fixture.Connect();
        fixture.SendAdmission(replacement, ServiceWireConstants.Command.Hello);
        await WaitUntilAsync(() =>
            fixture.Node.PeerChannels(fixture.PeerRid, Lifecycle).Length != 0
        );
        Assert.NotEqual(generation, fixture.Route.SelectedGeneration);
        Assert.Equal(0UL, fixture.Route.ReadyGeneration);
        Assert.True(fixture.Claims(generation, ServiceWireConstants.Command.ChannelSend));
        Assert.False(fixture.Claims(generation, ServiceWireConstants.Command.ReplyRelay));
        SendChannel(replacement, "pending");

        fixture.SendAdmission(replacement, ServiceWireConstants.Command.Admit);
        await WaitUntilAsync(() => fixture.Route.ReadyGeneration != 0);
        Assert.False(fixture.Claims(generation, ServiceWireConstants.Command.ChannelSend));
        SendChannel(replacement, "replacement");
        Assert.Equal(new[] { "replacement" }, await fixture.DeliveriesThroughReplacementAsync());
    }

    [Fact]
    public async Task HandshakeRequiresTheSelectedGeneration()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var rid = RoutingId.From("selected-peer");
        node.ApplySelectedRoutes([new RouterRoute(rid, 2)]);

        foreach (
            var command in new[]
            {
                ServiceWireConstants.Command.Hello,
                ServiceWireConstants.Command.Admit,
                ServiceWireConstants.Command.Reject,
            }
        )
        {
            Assert.False(node.TryClaimIngressSource(rid, 1, command, out _));
            Assert.True(node.TryClaimIngressSource(rid, 2, command, out _));
        }
    }

    [Fact]
    public async Task WithdrawalOnlyRetiresTheEndedAdmissionAtItsEndpoint()
    {
        await using var fixture = new Fixture();
        var generation = await fixture.AdmitAndCloseAsync();

        fixture.Node.RemovePeerExpectation(fixture.PeerRid, fixture.PeerEndpoint + "-other");
        Assert.True(fixture.Claims(generation, ServiceWireConstants.Command.ChannelSend));
        fixture.Node.RemovePeerExpectation(fixture.PeerRid, fixture.PeerEndpoint);
        Assert.False(fixture.Claims(generation, ServiceWireConstants.Command.ChannelSend));
    }

    [Fact]
    public async Task ChangedExpectedLifecycleRetiresTheEndedAdmission()
    {
        await using var fixture = new Fixture();
        var generation = await fixture.AdmitAndCloseAsync();
        fixture.Node.SetPeerExpectation(
            fixture.PeerRid,
            fixture.PeerEndpoint,
            ZLinkServiceSecurityIdentity.Plaintext,
            Lifecycle
        );
        Assert.True(fixture.Claims(generation, ServiceWireConstants.Command.ChannelSend));
        fixture.Node.SetPeerExpectation(
            fixture.PeerRid,
            fixture.PeerEndpoint,
            ZLinkServiceSecurityIdentity.Plaintext,
            Lifecycle + 1
        );
        Assert.False(fixture.Claims(generation, ServiceWireConstants.Command.ChannelSend));
    }

    [Fact]
    public async Task ExpiryOfPendingReplacementRetiresTheEndedAdmission()
    {
        await using var fixture = new Fixture();
        var generation = await fixture.AdmitAndCloseAsync();
        await using var replacement = fixture.Connect();
        fixture.SendAdmission(replacement, ServiceWireConstants.Command.Hello);
        await WaitUntilAsync(() =>
            fixture.Node.PeerChannels(fixture.PeerRid, Lifecycle).Length != 0
        );
        Assert.True(fixture.Claims(generation, ServiceWireConstants.Command.ChannelSend));

        var expiredAt = Stopwatch.GetTimestamp() + Stopwatch.Frequency * 60;
        fixture.Node.ProcessInfrastructure(expiredAt);
        Assert.False(fixture.Claims(generation, ServiceWireConstants.Command.ChannelSend));
    }

    private static void SendChannel(IDealerSocket dealer, string value)
    {
        using var head = ZLinkServiceWireCodec.EncodeApplicationMessage(
            ServiceWireConstants.Command.ChannelSend,
            0,
            ChannelName,
            false
        );
        var envelope = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Command,
            ChannelName,
            nameof(Order),
            ZLinkEnvelopeCodec.DefaultContentType,
            null,
            null,
            null,
            null,
            null
        );
        var parts = ZLinkEnvelopeCodec.EncodeParts(envelope, new Order(value), typeof(Order), null);
        try
        {
            using var payload =
                ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(parts);
            dealer.Send().Message(head).Message(payload).Submit();
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        using var timeout = new CancellationTokenSource(ObservationTimeout);
        while (!condition())
            await Task.Delay(1, timeout.Token);
    }

    private sealed record Order(string Value);

    private sealed class OrderHandler(Channel<string> deliveries) : IZLinkSendHandler<Order>
    {
        public ValueTask HandleAsync(
            Order message,
            IZLinkMessageContext context,
            CancellationToken cancellationToken
        )
        {
            deliveries.Writer.TryWrite(message.Value);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class Fixture : IAsyncDisposable
    {
        private readonly IContext _context = Systems.Zlink.Zlink.CreateContext();
        private readonly string _endpoint = $"inproc://ended-route-{Guid.NewGuid():N}";
        private readonly ZLinkMeshDispatchPump _pump;
        private readonly ServiceProvider _services;
        private readonly ZLinkRuntimeTaskRunner _runner;
        private readonly ZLinkRuntimeErrorSink _errors = new();
        private readonly Channel<string> _deliveries = Channel.CreateUnbounded<string>();
        internal readonly RoutingId PeerRid = RoutingId.From($"peer-{Guid.NewGuid():N}");
        internal readonly ZLinkManagedMeshNode Node;
        internal readonly ObservedRouter Router;
        internal ZLinkPeerRouteObservation Route => Node.ObservePeerRoute(PeerRid);
        internal string PeerEndpoint => _endpoint + "-peer";

        internal Fixture()
        {
            ObservedRouter? router = null;
            var context = new CapturingRouterContext(
                _context,
                native =>
                    router = new ObservedRouter(native, PeerRid, () => Route.SelectedGeneration)
            );
            Node = new ZLinkManagedMeshNode(
                context,
                MeshName,
                receivePollerFactory: () =>
                    new MeshReceiveWakeTests.ControlledReceivePoller(socket =>
                        socket is ObservedRouter decorated ? decorated.Native : socket
                    )
                    {
                        CaptureIdleWait = true,
                        BeforeWait = () => router?.ObserveReceiveTurnEnd(),
                    }
            );
            Node.SetRoutingId(RoutingId.From($"node-{Guid.NewGuid():N}"));
            Node.SetBind(_endpoint);
            Node.AddChannel(ChannelName);
            Node.Start();
            Router = router!;
            _pump = new ZLinkMeshDispatchPump(Node, new ZLinkMeshCompletionTable());
            _services = new ServiceCollection()
                .AddSingleton(_deliveries)
                .AddTransient<OrderHandler>()
                .BuildServiceProvider();
            var registration = new ZLinkFrameworkRegistration();
            var nodeRegistration = new ZLinkSpotNodeRegistration
            {
                SpotNodeName = MeshName,
                RoutingId = Node.RoutingId,
            };
            var channel = new ZLinkMeshChannelMembership { ChannelName = ChannelName };
            channel.SendHandlers.Add(new(typeof(OrderHandler), typeof(Order), null, nameof(Order)));
            nodeRegistration.ChannelMemberships.Add(channel);
            var runtime = new ZLinkFrameworkRuntime(
                _services,
                null!,
                registration,
                new ZLinkHandlerRegistry([]),
                new ZLinkHandlerDispatcher(
                    _services.GetRequiredService<IServiceScopeFactory>(),
                    registration
                )
            );
            _runner = new ZLinkRuntimeTaskRunner(_errors, CancellationToken.None);
            var dispatcher = ZLinkMeshNodeRouteDispatcher.Create(
                _services,
                registration,
                nodeRegistration,
                runtime,
                _runner
            )!;
            _pump.SetNodeRouteHandler(
                (records, cancellationToken) =>
                {
                    foreach (var record in records)
                    {
                        Assert.Equal(ChannelName, record.ChannelName);
                        Assert.Equal(PeerRid, record.SourceNodeRid);
                        Assert.Equal(Lifecycle, record.SourceNodeGeneration);
                    }
                    return dispatcher.DispatchBatchAsync(records, cancellationToken);
                },
                _runner
            );
            _pump.EnsureStarted();
        }

        internal IDealerSocket Connect()
        {
            var dealer = _context.CreateDealerSocket();
            dealer.SetRoutingId(PeerRid);
            dealer.Connect(_endpoint);
            return dealer;
        }

        internal void SendAdmission(IDealerSocket dealer, ServiceWireConstants.Command command)
        {
            using var head = Message.From(
                ZLinkServiceWireCodec.EncodeRouteAdmission(
                    command,
                    MeshName,
                    PeerEndpoint,
                    Lifecycle,
                    1,
                    new Dictionary<string, uint> { [ChannelName] = 100 }
                )
            );
            dealer.Send().Message(head).Submit();
        }

        internal async Task<ulong> AdmitAsync(IDealerSocket dealer)
        {
            SendAdmission(dealer, ServiceWireConstants.Command.Hello);
            SendAdmission(dealer, ServiceWireConstants.Command.Admit);
            await WaitUntilAsync(() => Route.ReadyGeneration != 0);
            return Route.ReadyGeneration;
        }

        internal async Task<ulong> AdmitAndCloseAsync()
        {
            await using var dealer = Connect();
            var generation = await AdmitAsync(dealer);
            dealer.Close();
            await WaitUntilAsync(() => Route.SelectedGeneration == 0);
            return generation;
        }

        internal bool Claims(ulong routeGeneration, ServiceWireConstants.Command command) =>
            Node.TryClaimIngressSource(PeerRid, routeGeneration, command, out _);

        internal Task<string> NextDeliveryAsync() =>
            _deliveries.Reader.ReadAsync().AsTask().WaitAsync(ObservationTimeout);

        internal async Task<string[]> DeliveriesThroughReplacementAsync()
        {
            var deliveries = new List<string>();
            string delivery;
            do
            {
                delivery = await NextDeliveryAsync();
                deliveries.Add(delivery);
            } while (delivery != "replacement");
            return deliveries.ToArray();
        }

        public async ValueTask DisposeAsync()
        {
            await _pump.DisposeAsync();
            await _runner.StopAsync();
            await Node.DisposeAsync();
            await _services.DisposeAsync();
            await _context.DisposeAsync();
            _errors.Dispose();
        }
    }

    private sealed class ObservedRouter(
        IRouterSocket inner,
        RoutingId peer,
        Func<ulong> appliedGeneration
    ) : PressureRouter(inner)
    {
        private sealed class Hold
        {
            internal readonly TaskCompletionSource Blocked = new(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            internal readonly TaskCompletionSource<(ulong Receipt, ulong Selected)> Receipt = new(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            internal bool RouteEnded;
        }

        private readonly TaskCompletionSource _currentDataDrained = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private bool _readCurrentData;
        private Hold? _hold;
        internal IRouterSocket Native => inner;
        internal Task CurrentDataDrained => _currentDataDrained.Task;

        internal void ObserveReceiveTurnEnd()
        {
            if (Volatile.Read(ref _readCurrentData))
                _currentDataDrained.TrySetResult();
        }

        internal (Task Blocked, Task<(ulong Receipt, ulong Selected)> Receipt) HoldUntilRouteEnds()
        {
            var hold = new Hold();
            Volatile.Write(ref _hold, hold);
            return (hold.Blocked.Task, hold.Receipt.Task);
        }

        public override IReadOnlyList<RouterRoute> RoutesSnapshot()
        {
            var routes = base.RoutesSnapshot();
            if (
                Volatile.Read(ref _hold) is { } hold
                && !routes.Any(route => route.RoutingId == peer)
            )
                hold.RouteEnded = true;
            return routes;
        }

        public override bool Recv(Received result, RecvFlags flags = RecvFlags.None)
        {
            var hold = Volatile.Read(ref _hold);
            if (hold is { RouteEnded: false })
            {
                hold.Blocked.TrySetResult();
                return false;
            }
            if (!base.Recv(result, flags))
                return false;
            if (
                hold is null
                && ZLinkServiceWireCodec.TryDecodePrefix(
                    result.FirstPart().AsReadOnlySpan(),
                    out var command,
                    out _,
                    out _
                )
                && command == ServiceWireConstants.Command.ChannelSend
            )
                Volatile.Write(ref _readCurrentData, true);
            if (hold is not null && result.RoutingId == peer)
            {
                Volatile.Write(ref _hold, null);
                hold.Receipt.TrySetResult((result.RouteGeneration, appliedGeneration()));
            }
            return true;
        }
    }
}
