using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using System.Net;
using System.Net.Sockets;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class RouteMeshRuntimeServiceTests
{
    [Fact]
    public async Task Missing_Required_Descriptor_Peer_Degrades_Topology()
    {
        await using var fixture = await RuntimeFixture.StartAsync(
            ZLinkMeshNodeObjectRole.Server);
        var remoteRid = RoutingId.From("aa-required-server");

        await fixture.PublishDescriptorAsync(
            remoteRid,
            ZLinkMeshNodeObjectRole.Server);

        var status = await WaitForStatusAsync(
            fixture.Runtime,
            candidate => candidate.Peers.Any(peer =>
                peer.NodeRid == remoteRid
                && peer.State == ZLinkPeerState.NotConnected));

        Assert.Equal(ZLinkTopologyState.Degraded, status.State);
        Assert.False(status.IsReady);
        Assert.True(status.Placement.IsAvailable);
    }

    [Fact]
    public async Task Zero_Placement_Weight_Disables_Placement()
    {
        await using var fixture = await RuntimeFixture.StartAsync(
            ZLinkMeshNodeObjectRole.Server,
            placementWeight: 0);

        var status = fixture.Runtime.GetStatus(RuntimeFixture.MeshName);

        Assert.False(status.Placement.IsAvailable);
        Assert.Equal(
            ZLinkTopologyReason.CapacityExceeded,
            status.Placement.UnavailableReason);
    }

    [Fact]
    public async Task Local_RouteMesh_Server_Is_Not_A_Ready_Target()
    {
        await using var fixture = await RuntimeFixture.StartAsync(
            ZLinkMeshNodeObjectRole.Server,
            registerServerChannel: true);

        var status = fixture.Runtime.GetStatus(RuntimeFixture.MeshName);
        var channel = Assert.Single(
            status.Channels,
            candidate => candidate.ChannelName == RuntimeFixture.MeshName);

        Assert.False(channel.IsReady);
        Assert.Equal(0, channel.ReadyTargetCount);

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            async () => await fixture.RouteClient
                .RequestToChannel(
                    RuntimeFixture.MeshName,
                    new RouteProbe("local-only"))
                .Async<RouteProbe>());

        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
    }

    [Fact]
    public async Task Remote_RouteMesh_Server_With_Zero_Weight_Is_NotFound()
    {
        var targetEndpoint = RuntimeFixture.ReserveTcpEndpoint();
        await using var target = await RuntimeFixture.StartAsync(
            ZLinkMeshNodeObjectRole.Server,
            routingIdPrefix: "zz-zero-weight-target",
            listenEndpoint: targetEndpoint,
            registerServerChannel: true,
            channelWeight: 0);
        await using var source = await RuntimeFixture.StartManualAsync(
            ZLinkMeshNodeObjectRole.Server,
            target.LocalNodeRid,
            target.ListenEndpoint,
            registerServerChannel: true);

        var status = await WaitForStatusAsync(
            source.Runtime,
            candidate => candidate.Peers.Any(peer =>
                peer.NodeRid == target.LocalNodeRid
                && peer.State == ZLinkPeerState.Ready));
        var channel = Assert.Single(
            status.Channels,
            candidate => candidate.ChannelName == RuntimeFixture.MeshName);
        Assert.False(channel.IsReady);
        Assert.Equal(0, channel.ReadyTargetCount);

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            async () => await source.RouteClient
                .RequestToChannel(
                    RuntimeFixture.MeshName,
                    new RouteProbe("zero-weight"))
                .Async<RouteProbe>());

        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
        Assert.Equal(ZLinkRetryAdvice.DoNotRetry, error.RetryAdvice);
    }

    [Fact]
    public async Task Previously_Admitted_RouteMesh_Server_When_Disconnected_Is_Unavailable()
    {
        var targetEndpoint = RuntimeFixture.ReserveTcpEndpoint();
        await using var target = await RuntimeFixture.StartAsync(
            ZLinkMeshNodeObjectRole.Server,
            routingIdPrefix: "zz-disconnected-target",
            listenEndpoint: targetEndpoint,
            registerServerChannel: true);
        await using var source = await RuntimeFixture.StartManualAsync(
            ZLinkMeshNodeObjectRole.Server,
            target.LocalNodeRid,
            target.ListenEndpoint,
            registerServerChannel: true);

        await WaitForStatusAsync(
            source.Runtime,
            candidate => candidate.Peers.Any(peer =>
                peer.NodeRid == target.LocalNodeRid
                && peer.State == ZLinkPeerState.Ready));

        await target.DisposeMeshNodeAsync();
        var disconnected = await WaitForStatusAsync(
            source.Runtime,
            candidate => candidate.Peers.Any(peer =>
                peer.NodeRid == target.LocalNodeRid
                && peer.State == ZLinkPeerState.Connecting),
            TimeSpan.FromSeconds(25));
        var channel = Assert.Single(
            disconnected.Channels,
            candidate => candidate.ChannelName == RuntimeFixture.MeshName);
        Assert.False(channel.IsReady);
        Assert.Equal(0, channel.ReadyTargetCount);

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            async () => await source.RouteClient
                .RequestToChannel(
                    RuntimeFixture.MeshName,
                    new RouteProbe("disconnected"))
                .Async<RouteProbe>());

        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
        Assert.Equal(ZLinkRetryAdvice.RetryAfterBackoff, error.RetryAdvice);
    }

    [Theory]
    [InlineData(0, 0, 0, true)]
    [InlineData(4, 5, 10, true)]
    [InlineData(5, 5, 10, false)]
    [InlineData(10, 1, 10, false)]
    public void Population_Capacity_Uses_Active_And_Reserved_Counts(
        int active,
        int reserved,
        int limit,
        bool expected)
    {
        var capacity = new ZLinkPopulationCapacity(active, reserved, limit);

        Assert.Equal(
            expected,
            ZLinkRouteMeshRuntimeService.HasRemainingCapacity(capacity));
    }

    [Theory]
    [InlineData(0, 0, true)]
    [InlineData(3, 4, true)]
    [InlineData(4, 4, false)]
    public void Placement_Uses_Activation_Concurrency(
        int active,
        int limit,
        bool expected)
    {
        Assert.Equal(
            expected,
            ZLinkRouteMeshRuntimeService.HasRemainingCapacity(
                new ZLinkActivationConcurrency(active, limit)));
    }

    [Fact]
    public async Task Host_Relocation_Disables_Public_Readiness_But_Keeps_Physical_Counts()
    {
        await using var fixture = await RuntimeFixture.StartAsync(
            ZLinkMeshNodeObjectRole.Server);
        var serving = fixture.Runtime.GetStatus(RuntimeFixture.MeshName);

        fixture.SetHostState(ZLinkFrameworkRuntimeState.Relocating);
        var relocating = fixture.Runtime.GetStatus(RuntimeFixture.MeshName);

        Assert.False(relocating.IsReady);
        Assert.Equal(ZLinkTopologyState.Stopping, relocating.State);
        Assert.Equal(serving.ReadyPeerCount, relocating.ReadyPeerCount);
        Assert.Equal(
            serving.Channels.Select(static channel => channel.ReadyTargetCount),
            relocating.Channels.Select(static channel => channel.ReadyTargetCount));
        Assert.False(relocating.Placement.IsAvailable);

        fixture.SetHostState(ZLinkFrameworkRuntimeState.Relocated);
        Assert.Equal(
            ZLinkTopologyState.Stopping,
            fixture.Runtime.GetStatus(RuntimeFixture.MeshName).State);
    }

    [Fact]
    public async Task Stop_Preserves_Terminal_Status_Without_Waiting_For_Slow_Observer()
    {
        await using var fixture = await RuntimeFixture.StartAsync(
            ZLinkMeshNodeObjectRole.Server);
        using var observerStop = new CancellationTokenSource();
        await using var observer = fixture.Runtime
            .ObserveAsync(RuntimeFixture.MeshName, observerStop.Token)
            .GetAsyncEnumerator(observerStop.Token);
        Assert.True(await observer.MoveNextAsync());

        for (var index = 0; index < 1100; index++)
            fixture.SetHostState(index % 2 == 0
                ? ZLinkFrameworkRuntimeState.Relocating
                : ZLinkFrameworkRuntimeState.Serving);

        await Task.Run(fixture.StopMonitoring)
            .WaitAsync(TimeSpan.FromSeconds(2));

        Assert.True(await observer.MoveNextAsync());
        Assert.Equal(ZLinkTopologyState.Stopped, observer.Current.Status.State);
        Assert.False(observer.Current.Status.IsReady);

        var pending = observer.MoveNextAsync().AsTask();
        await Task.Delay(100);
        Assert.False(pending.IsCompleted);
        observerStop.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            async () => await pending);
    }

    [Fact]
    public async Task Concurrent_Change_And_Stop_Converge_To_Preserved_Terminal()
    {
        await using var fixture = await RuntimeFixture.StartAsync(
            ZLinkMeshNodeObjectRole.Server);
        using var observerStop = new CancellationTokenSource();
        await using var observer = fixture.Runtime
            .ObserveAsync(RuntimeFixture.MeshName, observerStop.Token)
            .GetAsyncEnumerator(observerStop.Token);
        Assert.True(await observer.MoveNextAsync());

        fixture.SetHostState(ZLinkFrameworkRuntimeState.Relocating);
        var pending = observer.MoveNextAsync().AsTask();
        await Task.Run(fixture.StopMonitoring)
            .WaitAsync(TimeSpan.FromSeconds(2));

        Assert.True(await pending);
        while (observer.Current.Status.State != ZLinkTopologyState.Stopped)
            Assert.True(await observer.MoveNextAsync());
        Assert.Equal(ZLinkTopologyState.Stopped, observer.Current.Status.State);
        Assert.False(observer.Current.Status.IsReady);
        var pendingAfterTerminal = observer.MoveNextAsync().AsTask();
        await Task.Delay(100);
        Assert.False(pendingAfterTerminal.IsCompleted);
        observerStop.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            async () => await pendingAfterTerminal);
    }

    [Fact]
    public async Task Missing_ObjectClient_Descriptor_Is_NotRequired_And_Remains_Ready()
    {
        await using var fixture = await RuntimeFixture.StartAsync(
            ZLinkMeshNodeObjectRole.Client);
        var remoteRid = RoutingId.From("aa-client");

        await fixture.PublishDescriptorAsync(
            remoteRid,
            ZLinkMeshNodeObjectRole.Client);

        var status = await WaitForStatusAsync(
            fixture.Runtime,
            candidate => candidate.Peers.Any(peer =>
                peer.NodeRid == remoteRid
                && peer.State == ZLinkPeerState.NotRequired));

        Assert.Equal(ZLinkTopologyState.Ready, status.State);
        Assert.True(status.IsReady);
        Assert.Equal(0, status.ReadyPeerCount);
    }

    [Fact]
    public async Task Automatic_ObjectClient_Target_Is_NotFound_Without_New_Connection_Intent()
    {
        await using var fixture = await RuntimeFixture.StartAsync(
            ZLinkMeshNodeObjectRole.Client);
        var remoteRid = RoutingId.From("aa-client-target");

        await fixture.PublishDescriptorAsync(
            remoteRid,
            ZLinkMeshNodeObjectRole.Client);
        await WaitForStatusAsync(
            fixture.Runtime,
            candidate => candidate.Peers.Any(peer =>
                peer.NodeRid == remoteRid
                && peer.State == ZLinkPeerState.NotRequired));
        var peerCount = fixture.Runtime.GetStatus(RuntimeFixture.MeshName).Peers.Count;

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            async () => await fixture.RouteClient
                .RequestToNode(
                    RuntimeFixture.MeshName,
                    remoteRid,
                    new RouteProbe("automatic"))
                .Async<RouteProbe>());
        var sendError = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            async () => await fixture.RouteClient
                .SendToNode(
                    RuntimeFixture.MeshName,
                    remoteRid,
                    new RouteProbe("automatic-send"))
                .Async());

        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
        Assert.Equal(
            ZLinkFrameworkErrorKind.NotFound,
            sendError.Kind);
        await Task.Delay(100);
        var status = fixture.Runtime.GetStatus(RuntimeFixture.MeshName);
        Assert.Equal(peerCount, status.Peers.Count);
        Assert.Single(
            status.Peers,
            peer => peer.NodeRid == remoteRid
                    && peer.State == ZLinkPeerState.NotRequired);
    }

    [Fact]
    public async Task Required_Server_To_Client_Peer_Is_NotConnected_But_Not_A_Node_Target()
    {
        await using var fixture = await RuntimeFixture.StartAsync(
            ZLinkMeshNodeObjectRole.Server);
        var remoteRid = RoutingId.From("aa-required-client");

        await fixture.PublishDescriptorAsync(
            remoteRid,
            ZLinkMeshNodeObjectRole.Client);
        var status = await WaitForStatusAsync(
            fixture.Runtime,
            candidate => candidate.Peers.Any(peer =>
                peer.NodeRid == remoteRid
                && peer.State == ZLinkPeerState.NotConnected));

        Assert.Equal(ZLinkTopologyState.Degraded, status.State);
        Assert.DoesNotContain(
            status.Peers,
            peer => peer.NodeRid == remoteRid
                    && peer.State == ZLinkPeerState.NotRequired);

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            async () => await fixture.RouteClient
                .RequestToNode(
                    RuntimeFixture.MeshName,
                    remoteRid,
                    new RouteProbe("required-client"))
                .Async<RouteProbe>());

        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
    }

    [Fact]
    public async Task Connecting_Expected_Peer_Has_One_Public_Identity()
    {
        await using var fixture = await RuntimeFixture.StartAsync(
            ZLinkMeshNodeObjectRole.Server,
            routingIdPrefix: "aa-runtime");
        var remoteRid = RoutingId.From("zz-required-server");

        await fixture.PublishDescriptorAsync(
            remoteRid,
            ZLinkMeshNodeObjectRole.Server);

        var status = await WaitForStatusAsync(
            fixture.Runtime,
            candidate => candidate.Peers.Any(peer =>
                peer.NodeRid == remoteRid
                && peer.State == ZLinkPeerState.Connecting));

        Assert.Single(status.Peers, peer => peer.NodeRid == remoteRid);
        Assert.DoesNotContain(status.Peers, peer => peer.NodeRid.IsEmpty);
        Assert.Equal(ZLinkTopologyState.Degraded, status.State);
    }

    [Fact]
    public async Task Manual_ObjectClient_Target_Is_NotFound_Without_Retrying_The_Pair()
    {
        var targetEndpoint = RuntimeFixture.ReserveTcpEndpoint();
        await using var target = await RuntimeFixture.StartAsync(
            ZLinkMeshNodeObjectRole.Client,
            listenEndpoint: targetEndpoint);
        await using var source = await RuntimeFixture.StartManualAsync(
            ZLinkMeshNodeObjectRole.Client,
            target.LocalNodeRid,
            target.ListenEndpoint);

        await WaitForStatusAsync(
            source.Runtime,
            candidate => candidate.Peers.Any(peer =>
                peer.NodeRid == target.LocalNodeRid
                && peer.State == ZLinkPeerState.NotRequired));
        var peerCount = source.Runtime.GetStatus(RuntimeFixture.MeshName).Peers.Count;

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            async () => await source.RouteClient
                .RequestToNode(
                    RuntimeFixture.MeshName,
                    target.LocalNodeRid,
                    new RouteProbe("manual"))
                .Async<RouteProbe>());

        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
        await Task.Delay(1100);
        var status = source.Runtime.GetStatus(RuntimeFixture.MeshName);
        Assert.Equal(peerCount, status.Peers.Count);
        Assert.Single(
            status.Peers,
            peer => peer.NodeRid == target.LocalNodeRid
                    && peer.State == ZLinkPeerState.NotRequired);
    }

    [Fact]
    public async Task Descriptor_Add_And_Remove_Wake_ObserveAsync()
    {
        await using var fixture = await RuntimeFixture.StartAsync(
            ZLinkMeshNodeObjectRole.Client);
        var remoteRid = RoutingId.From("aa-observed-client");
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        await using var observer = fixture.Runtime
            .ObserveAsync(RuntimeFixture.MeshName, timeout.Token)
            .GetAsyncEnumerator(timeout.Token);

        Assert.True(await observer.MoveNextAsync());
        await fixture.PublishDescriptorAsync(
            remoteRid,
            ZLinkMeshNodeObjectRole.Client);
        var added = await MoveUntilAsync(
            observer,
            status => status.Status.Peers.Any(peer => peer.NodeRid == remoteRid));
        Assert.Contains(
            added.Status.Peers,
            peer => peer.NodeRid == remoteRid
                    && peer.State == ZLinkPeerState.NotRequired);

        await fixture.RemoveDescriptorAsync(remoteRid);
        var removed = await MoveUntilAsync(
            observer,
            status => status.Status.Peers.All(peer => peer.NodeRid != remoteRid));
        Assert.DoesNotContain(removed.Status.Peers, peer => peer.NodeRid == remoteRid);
    }

    [Fact]
    public async Task Location_Health_Degrades_And_Recovers_Status_Stream()
    {
        await using var fixture = await RuntimeFixture.StartAsync(
            ZLinkMeshNodeObjectRole.Server);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        await using var observer = fixture.Runtime
            .ObserveAsync(RuntimeFixture.MeshName, timeout.Token)
            .GetAsyncEnumerator(timeout.Token);

        Assert.True(await observer.MoveNextAsync());
        fixture.ReportLocationFailure();
        var degraded = await MoveUntilAsync(
            observer,
            status => status.Status.State == ZLinkTopologyState.Degraded);
        Assert.False(degraded.Status.IsReady);
        Assert.False(degraded.Status.Placement.IsAvailable);
        Assert.Equal(
            ZLinkTopologyReason.LocationUnavailable,
            degraded.Status.Placement.UnavailableReason);

        fixture.ReportLocationSuccess();
        var recovered = await MoveUntilAsync(
            observer,
            status => status.Status.State == ZLinkTopologyState.Ready);
        Assert.True(recovered.Status.IsReady);
        Assert.True(recovered.Status.Placement.IsAvailable);
    }

    [Fact]
    public async Task Placement_Weight_Change_And_Recovery_Wake_Status_Stream()
    {
        await using var fixture = await RuntimeFixture.StartAsync(
            ZLinkMeshNodeObjectRole.Server);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        await using var observer = fixture.Runtime
            .ObserveAsync(RuntimeFixture.MeshName, timeout.Token)
            .GetAsyncEnumerator(timeout.Token);

        Assert.True(await observer.MoveNextAsync());
        fixture.RuntimeOptions.Mesh(RuntimeFixture.MeshName).PlacementWeight = 0;
        var unavailable = await MoveUntilAsync(
            observer,
            status => !status.Status.Placement.IsAvailable);
        Assert.Equal(
            ZLinkTopologyReason.CapacityExceeded,
            unavailable.Status.Placement.UnavailableReason);

        fixture.RuntimeOptions.Mesh(RuntimeFixture.MeshName).PlacementWeight = 100;
        var recovered = await MoveUntilAsync(
            observer,
            status => status.Status.Placement.IsAvailable);
        Assert.True(recovered.Status.Placement.IsAvailable);
    }

    private static async Task<ZLinkRouteMeshStatus> WaitForStatusAsync(
        IZLinkRouteMeshRuntime runtime,
        Func<ZLinkRouteMeshStatus, bool> predicate,
        TimeSpan? timeout = null)
    {
        var deadline = DateTimeOffset.UtcNow + (timeout ?? TimeSpan.FromSeconds(5));
        while (DateTimeOffset.UtcNow < deadline)
        {
            var status = runtime.GetStatus(RuntimeFixture.MeshName);
            if (predicate(status))
                return status;
            await Task.Delay(10);
        }
        var current = runtime.GetStatus(RuntimeFixture.MeshName);
        throw new TimeoutException(
            "RouteMesh status did not reach the expected state. "
            + string.Join(
                ", ",
                current.Peers.Select(peer =>
                    $"{peer.NodeRid}:{peer.State}")));
    }

    private static async Task<ZLinkObservedStatus<ZLinkRouteMeshStatus>> MoveUntilAsync(
        IAsyncEnumerator<ZLinkObservedStatus<ZLinkRouteMeshStatus>> observer,
        Func<ZLinkObservedStatus<ZLinkRouteMeshStatus>, bool> predicate)
    {
        while (await observer.MoveNextAsync())
        {
            if (predicate(observer.Current))
                return observer.Current;
        }
        throw new InvalidOperationException(
            "RouteMesh observation completed before the expected status.");
    }

    private sealed class RuntimeFixture : IAsyncDisposable
    {
        internal const string MeshName = "orders";

        private readonly ServiceProvider _provider;
        private readonly IHostedService _hosted;
        private readonly ZLinkLocationRuntime _locations;
        private readonly ZLinkLocationStoreHealth _locationHealth;
        private readonly ZLinkFrameworkHostLifecycleState _hostLifecycle;
        private readonly ZLinkRouteMeshRuntimeService _monitoring;
        private bool _stopped;

        private RuntimeFixture(
            ServiceProvider provider,
            IHostedService hosted,
            ZLinkLocationRuntime locations,
            ZLinkLocationStoreHealth locationHealth,
            ZLinkFrameworkHostLifecycleState hostLifecycle,
            ZLinkRouteMeshRuntimeService monitoring,
            IZLinkRouteMeshRuntime runtime,
            IZLinkRouteMeshRuntimeOptions runtimeOptions,
            IZLinkRouteClient routeClient,
            RoutingId localNodeRid,
            string listenEndpoint)
        {
            _provider = provider;
            _hosted = hosted;
            _locations = locations;
            _locationHealth = locationHealth;
            _hostLifecycle = hostLifecycle;
            _monitoring = monitoring;
            Runtime = runtime;
            RuntimeOptions = runtimeOptions;
            RouteClient = routeClient;
            LocalNodeRid = localNodeRid;
            ListenEndpoint = listenEndpoint;
        }

        internal IZLinkRouteMeshRuntime Runtime { get; }

        internal IZLinkRouteMeshRuntimeOptions RuntimeOptions { get; }

        internal IZLinkRouteClient RouteClient { get; }

        internal RoutingId LocalNodeRid { get; }

        internal string ListenEndpoint { get; }

        internal static async Task<RuntimeFixture> StartAsync(
            ZLinkMeshNodeObjectRole objectRole,
            string routingIdPrefix = "zz-runtime",
            string? listenEndpoint = null,
            int placementWeight = 100,
            bool registerServerChannel = false,
            int channelWeight = 100)
        {
            listenEndpoint ??= $"inproc://route-runtime-{Guid.NewGuid():N}";
            var services = new ServiceCollection();
            services.AddZLinkFramework(options =>
            {
                options.ConfigureInboundDispatch().ApplicationHwmBytes = 0;
                options.UseTestLocationStore();
                var node = options.AddRouteMesh(MeshName)
                    .Listen(listenEndpoint)
                    .SetRoutingIdPrefix(routingIdPrefix)
                    .SetPlacementWeight(placementWeight);
                if (registerServerChannel)
                    node.Channel(MeshName).Server().SetWeight(channelWeight);
                if (objectRole == ZLinkMeshNodeObjectRole.Client)
                    node.Objects().Client();
                else
                    node.Objects().Server();
            });
            return await StartProviderAsync(services, listenEndpoint);
        }

        internal static async Task<RuntimeFixture> StartManualAsync(
            ZLinkMeshNodeObjectRole objectRole,
            RoutingId peerRid,
            string peerEndpoint,
            bool registerServerChannel = false,
            int channelWeight = 100)
        {
            var listenEndpoint = ReserveTcpEndpoint();
            var services = new ServiceCollection();
            services.AddZLinkFramework(options =>
            {
                options.ConfigureInboundDispatch().ApplicationHwmBytes = 0;
                options.UseTestLocationStore();
                var node = options.AddRouteMesh(MeshName)
                    .Listen(listenEndpoint);
                if (registerServerChannel)
                    node.Channel(MeshName).Server().SetWeight(channelWeight);
                if (objectRole == ZLinkMeshNodeObjectRole.Client)
                    node.Objects().Client();
                else
                    node.Objects().Server();
                node.PeerConnections.Connect(peerRid, peerEndpoint);
            });
            return await StartProviderAsync(services, listenEndpoint);
        }

        internal static string ReserveTcpEndpoint()
        {
            var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            var port = ((IPEndPoint)listener.LocalEndpoint).Port;
            listener.Stop();
            return $"tcp://127.0.0.1:{port}";
        }

        private static async Task<RuntimeFixture> StartProviderAsync(
            ServiceCollection services,
            string listenEndpoint)
        {
            var provider = services.BuildServiceProvider();
            var hosted = provider.GetServices<IHostedService>().Single(
                static service => service is ZLinkFrameworkHostedService);
            try
            {
                await hosted.StartAsync(CancellationToken.None);
                var frameworkRuntime =
                    provider.GetRequiredService<ZLinkFrameworkRuntime>();
                return new RuntimeFixture(
                    provider,
                    hosted,
                    provider.GetRequiredService<ZLinkLocationRuntime>(),
                    provider.GetRequiredService<ZLinkLocationStoreHealth>(),
                    provider.GetRequiredService<ZLinkFrameworkHostLifecycleState>(),
                    provider.GetRequiredService<ZLinkRouteMeshRuntimeService>(),
                    provider.GetRequiredService<IZLinkRouteMeshRuntime>(),
                    provider.GetRequiredService<IZLinkRouteMeshRuntimeOptions>(),
                    provider.GetRequiredService<IZLinkRouteClient>(),
                    frameworkRuntime.GetMeshNodeRuntime(MeshName).Node.RoutingId,
                    listenEndpoint);
            }
            catch
            {
                await provider.DisposeAsync();
                throw;
            }
        }

        internal async Task PublishDescriptorAsync(
            RoutingId rid,
            ZLinkMeshNodeObjectRole objectRole)
        {
            var result = await _locations.WriteDescriptorAsync(
                new ZLinkMeshNodeDescriptor(
                    MeshName,
                    rid,
                    LifecycleGeneration: 1,
                    DescriptorRevision: 1,
                    $"inproc://missing-{Guid.NewGuid():N}",
                    new Dictionary<string, int>(StringComparer.Ordinal),
                    SecurityIdentity: ZLinkTransportSecurityIdentity.Plaintext,
                    OwnerId: string.Empty,
                    LeaseGeneration: 0,
                    UpdatedAt: default)
                {
                    ObjectRole = objectRole,
                    State = ZLinkFrameworkRuntimeState.Serving,
                    EntrySpotId = objectRole == ZLinkMeshNodeObjectRole.Server
                        ? $"{rid.ToHex()}-entry"
                        : null
                },
                ZLinkLocationWriteIntent.NewClaim);
            Assert.Equal(ZLinkLocationWriteStatus.Stored, result.Status);
        }

        internal async Task RemoveDescriptorAsync(RoutingId rid)
        {
            var result = await _locations.RemoveDescriptorAsync(
                new ZLinkMeshNodeDescriptorKey(MeshName, rid));
            Assert.Equal(ZLinkLocationWriteStatus.Stored, result.Status);
        }

        internal void ReportLocationFailure() =>
            _locationHealth.ReportFailure(
                "route-mesh-runtime-test",
                new InvalidOperationException("location unavailable"));

        internal void ReportLocationSuccess() =>
            _locationHealth.ReportSuccess("route-mesh-runtime-test");

        internal void SetHostState(ZLinkFrameworkRuntimeState state) =>
            _hostLifecycle.TransitionTo(state);

        internal void StopMonitoring() => _monitoring.Stop();

        internal async Task StopAsync()
        {
            if (_stopped)
                return;
            _stopped = true;
            await _hosted.StopAsync(CancellationToken.None);
        }

        internal async Task DisposeMeshNodeAsync()
        {
            await _provider
                .GetRequiredService<ZLinkFrameworkRuntime>()
                .GetMeshNodeRuntime(MeshName)
                .DisposeAsync();
        }

        public async ValueTask DisposeAsync()
        {
            try
            {
                await StopAsync();
            }
            finally
            {
                await _provider.DisposeAsync();
            }
        }
    }

    private sealed record RouteProbe(string Value);
}
