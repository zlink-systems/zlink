using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Configuration;

namespace Zlink.Framework.UnitTests;

public sealed class ListenerIdentityAndNodeDirectTests
{
    [Theory]
    [InlineData("0.0.0.0", "tcp://127.0.0.1:7101")]
    [InlineData("::", "tcp://[::1]:7101")]
    public void Wildcard_bind_without_advertise_host_uses_same_family_loopback(
        string bindHost,
        string expected)
    {
        var network = new ZLinkNetworkOptionsModel();
        var boundEndpoint = ZLinkNetworkEndpointResolver.Bind(
            explicitEndpoint: null,
            port: 7101,
            listenerBindHost: bindHost,
            network);
        var advertised = ZLinkNetworkEndpointResolver.Advertise(
            boundEndpoint,
            listenerAdvertiseHost: null,
            listenerBindHost: bindHost,
            network);

        Assert.Equal(expected, advertised);
    }

    [Theory]
    [InlineData("0.0.0.0")]
    [InlineData("::")]
    public async Task Explicit_wildcard_advertise_host_fails_startup(string advertiseHost)
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders();
        builder.Services.AddZLinkFramework(options =>
        {
            options.AddRouteMesh("invalid-advertise-mesh")
                .Listen()
                .SetAdvertiseHost(advertiseHost)
                .SetRoutingId(RoutingId.From("invalid-advertise-node"))
                .AddRouteRequestHandler<ProbeHandler, ProbeRequest, ProbeReply>();
        });
        using var host = builder.Build();

        var exception = await Assert.ThrowsAsync<ZLinkConfigurationException>(
            () => host.StartAsync());
        Assert.Contains("AdvertiseHost must not be a wildcard", exception.Message);
    }

    [Fact]
    public async Task All_listener_kinds_start_with_wildcard_bind_and_default_advertise_host()
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders();
        builder.Services.AddZLinkFramework(options =>
        {
            options.ConfigureNetwork().BindHost = "0.0.0.0";
            options.AddRouteMesh("listener-mesh")
                .Listen()
                .SetRoutingId(RoutingId.From("listener-mesh-node"))
                .AddRouteRequestHandler<ProbeHandler, ProbeRequest, ProbeReply>();
            options.AddClientServerChannel("listener-client-server")
                .Server()
                .Listen()
                .AddRequestHandler<ProbeHandler, ProbeRequest, ProbeReply>();
            options.AddFanoutChannel("listener-fanout").EnablePublisher();
            options.AddStreamNode("listener-stream")
                .Bind()
                .AddSession<TestSession>();
        });
        using var host = builder.Build();

        await host.StartAsync();
        await host.StopAsync();
    }

    [Fact]
    public async Task Wildcard_listener_advertises_loopback_to_expected_route_and_node_direct_succeeds()
    {
        var port = ReserveTcpPort();
        var targetRid = RoutingId.From("wildcard-target");
        using var target = BuildMeshHost(
            RoutingId.From("wildcard-target"),
            port,
            bindHost: "0.0.0.0");
        await target.StartAsync();
        using var source = BuildMeshHost(
            RoutingId.From("wildcard-source"),
            ReserveTcpPort(),
            expectedPeerRid: targetRid,
            peerEndpoint: $"tcp://127.0.0.1:{port}");
        await source.StartAsync();
        try
        {
            await WaitForPeerAsync(source, targetRid);
            var reply = await source.Services.GetRequiredService<IZLinkRouteClient>()
                .RequestToNode("listener-node-direct", targetRid, new ProbeRequest("ping"))
                .Async<ProbeReply>();

            Assert.Equal("ping-reply", reply.Value);
        }
        finally
        {
            await source.StopAsync();
            await target.StopAsync();
        }
    }

    [Fact]
    public async Task Endpoint_only_admitted_peer_is_a_node_direct_target()
    {
        var port = ReserveTcpPort();
        var targetRid = RoutingId.From("endpoint-only-target");
        using var target = BuildMeshHost(targetRid, port);
        await target.StartAsync();
        using var source = BuildMeshHost(
            RoutingId.From("endpoint-only-source"),
            ReserveTcpPort(),
            peerEndpoint: $"tcp://127.0.0.1:{port}");
        await source.StartAsync();
        try
        {
            await WaitForPeerAsync(source, targetRid);
            var reply = await source.Services.GetRequiredService<IZLinkRouteClient>()
                .RequestToNode("listener-node-direct", targetRid, new ProbeRequest("endpoint"))
                .Async<ProbeReply>();

            Assert.Equal("endpoint-reply", reply.Value);
        }
        finally
        {
            await source.StopAsync();
            await target.StopAsync();
        }
    }

    [Fact]
    public async Task Expected_route_rejection_is_logged_at_warning_with_both_endpoints()
    {
        var logger = new RecordingMeshNodeLogger();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = new ZLinkManagedMeshNode(context, "warning-mesh");
        await using var target = new ZLinkManagedMeshNode(
            context,
            "warning-mesh",
            logger: logger);
        var suffix = Guid.NewGuid().ToString("N");
        var sourceRid = RoutingId.From($"warning-source-{suffix}");
        var targetRid = RoutingId.From($"warning-target-{suffix}");
        var sourceEndpoint = $"inproc://warning-source-{suffix}";
        var targetEndpoint = $"inproc://warning-target-{suffix}";
        var intentEndpoint = $"inproc://unexpected-{suffix}";
        source.SetRoutingId(sourceRid);
        source.SetBind(sourceEndpoint);
        source.ConnectPeer(targetEndpoint);
        target.SetRoutingId(targetRid);
        target.SetBind(targetEndpoint);
        target.SetPeerExpectation(
            sourceRid,
            intentEndpoint,
            ZLinkServiceSecurityIdentity.Plaintext,
            source.Status().LifecycleGeneration);

        target.Start();
        source.Start();

        await WaitUntilAsync(() => logger.Entries.Any());
        var entry = Assert.Single(logger.Entries);
        Assert.Equal(LogLevel.Warning, entry.Level);
        Assert.Contains("reason=route_mismatch", entry.Message, StringComparison.Ordinal);
        Assert.Contains($"intent_endpoint={intentEndpoint}", entry.Message, StringComparison.Ordinal);
        Assert.Contains($"advertised_endpoint={sourceEndpoint}", entry.Message, StringComparison.Ordinal);
    }

    private static IHost BuildMeshHost(
        RoutingId routingId,
        int listenPort,
        string bindHost = "127.0.0.1",
        RoutingId? expectedPeerRid = null,
        string? peerEndpoint = null)
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders();
        builder.Services.AddZLinkFramework(options =>
        {
            var mesh = options.AddRouteMesh("listener-node-direct")
                .Listen(listenPort)
                .SetBindHost(bindHost)
                .SetRoutingId(routingId)
                .AddRouteRequestHandler<ProbeHandler, ProbeRequest, ProbeReply>();
            if (peerEndpoint is null) return;
            if (expectedPeerRid is { } expected)
                mesh.PeerConnections.Connect(expected, peerEndpoint);
            else
                mesh.PeerConnections.Connect(peerEndpoint);
        });
        return builder.Build();
    }

    private static async Task WaitForPeerAsync(IHost host, RoutingId peerRid)
    {
        var runtime = host.Services.GetRequiredService<IZLinkRouteMeshRuntime>();
        await WaitUntilAsync(() => runtime.GetStatus("listener-node-direct")
            .Peers.Any(peer => peer.NodeRid == peerRid && peer.State == ZLinkPeerState.Ready));
    }

    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        var deadline = Stopwatch.GetTimestamp() + Stopwatch.Frequency * 5;
        while (Stopwatch.GetTimestamp() < deadline)
        {
            if (condition()) return;
            await Task.Delay(10);
        }
        Assert.Fail("The expected runtime condition was not reached.");
    }

    private static int ReserveTcpPort()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        try
        {
            return ((IPEndPoint)listener.LocalEndpoint).Port;
        }
        finally
        {
            listener.Stop();
        }
    }

    public sealed record ProbeRequest(string Value);

    public sealed record ProbeReply(string Value);

    public sealed class ProbeHandler :
        IZLinkRouteRequestHandler<ProbeRequest, ProbeReply>,
        IZLinkRequestHandler<ProbeRequest, ProbeReply>
    {
        ValueTask<ProbeReply> IZLinkRouteRequestHandler<ProbeRequest, ProbeReply>.HandleAsync(
            ProbeRequest request,
            ZLinkRouteMessageContext context,
            CancellationToken cancellationToken) =>
            ValueTask.FromResult(new ProbeReply($"{request.Value}-reply"));

        public ValueTask<ProbeReply> HandleAsync(
            ProbeRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken) =>
            ValueTask.FromResult(new ProbeReply($"{request.Value}-reply"));
    }

    private sealed class RecordingMeshNodeLogger : ILogger<ZLinkManagedMeshNode>
    {
        internal ConcurrentQueue<(LogLevel Level, string Message)> Entries { get; } = new();

        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) => true;

        public void Log<TState>(
            LogLevel logLevel,
            EventId eventId,
            TState state,
            Exception? exception,
            Func<TState, Exception?, string> formatter) =>
            Entries.Enqueue((logLevel, formatter(state, exception)));
    }
}
