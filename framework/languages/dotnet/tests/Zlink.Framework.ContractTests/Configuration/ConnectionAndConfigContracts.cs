using Microsoft.Extensions.Logging;
using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Configuration;

public sealed class ConnectionAndConfigContracts
{
    [Fact]
    [ContractExample(
        typeof(IZLinkSocketConfig),
        typeof(IZLinkRouteConfig),
        typeof(IZLinkOutboundRouteConfig),
        typeof(IZLinkSpotPublisherConfig),
        typeof(IZLinkSpotSubscriberConfig),
        typeof(IZLinkDispatchOptions),
        typeof(IZLinkUnhandledDispatchOptions),
        typeof(IZLinkDiagnosticsOptions))]
    public void Configuration_contracts_keep_socket_routing_spot_and_dispatch_options_typed()
    {
        var socket = new SocketConfig
        {
            MaxMessageSize = 1024,
            SendHighWaterMark = 10,
            ReceiveHighWaterMark = 20,
            SendBufferSize = 4096,
            ReceiveBufferSize = 8192,
            Linger = TimeSpan.Zero,
            ReceiveTimeout = TimeSpan.FromSeconds(1),
            SendTimeout = TimeSpan.FromSeconds(2),
            ConnectTimeout = TimeSpan.FromSeconds(3),
            HandshakeInterval = TimeSpan.FromSeconds(4),
            IPv6 = true,
            TcpNoDelay = true,
            Immediate = true
        };

        var route = new RouteConfig
        {
            RequireKnownPeer = true,
            AllowPeerHandover = true,
            EnablePeerProbe = true,
            ConnectRoutingId = RoutingId.From("peer")
        };

        var outbound = new OutboundRouteConfig
        {
            ProbeRouterOnConnect = true
        };

        IZLinkSpotPublisherConfig publisher = new SpotPublisherConfig
        {
            SendHighWaterMark = 32,
            SendTimeout = TimeSpan.FromMilliseconds(20),
            Linger = TimeSpan.Zero
        };

        IZLinkSpotSubscriberConfig subscriber = new SpotSubscriberConfig
        {
            ReceiveHighWaterMark = 64,
            ReceiveTimeout = TimeSpan.FromMilliseconds(30),
            Linger = TimeSpan.Zero
        };

        var dispatch = new DispatchOptions();

        Assert.True(socket.Immediate);
        Assert.True(route.RequireKnownPeer);
        Assert.True(outbound.ProbeRouterOnConnect);
        Assert.Equal(32UL, publisher.SendHighWaterMark);
        Assert.Equal(TimeSpan.FromMilliseconds(20), publisher.SendTimeout);
        Assert.Equal(TimeSpan.Zero, publisher.Linger);
        Assert.Equal(64UL, subscriber.ReceiveHighWaterMark);
        Assert.Equal(TimeSpan.FromMilliseconds(30), subscriber.ReceiveTimeout);
        Assert.Equal(TimeSpan.Zero, subscriber.Linger);
        Assert.NotNull(dispatch.Unhandled);
    }

    internal sealed class SocketConfig : IZLinkSocketConfig
    {
        public long MaxMessageSize { get; set; }

        public ulong SendHighWaterMark { get; set; }

        public ulong ReceiveHighWaterMark { get; set; }

        public int SendBufferSize { get; set; }

        public int ReceiveBufferSize { get; set; }

        public TimeSpan? Linger { get; set; }

        public TimeSpan? ReceiveTimeout { get; set; }

        public TimeSpan? SendTimeout { get; set; }

        public TimeSpan? ConnectTimeout { get; set; }

        public TimeSpan? HandshakeInterval { get; set; }

        public int Weight { get; set; }

        public bool IPv6 { get; set; }

        public bool TcpNoDelay { get; set; }

        public bool Immediate { get; set; }
    }

    internal sealed class RouteConfig : IZLinkRouteConfig
    {
        public bool RequireKnownPeer { get; set; }

        public bool AllowPeerHandover { get; set; }

        public bool EnablePeerProbe { get; set; }

        public RoutingId ConnectRoutingId { get; set; }
    }

    internal sealed class OutboundRouteConfig : IZLinkOutboundRouteConfig
    {
        public bool ProbeRouterOnConnect { get; set; }
    }

    internal sealed class SpotPublisherConfig : IZLinkSpotPublisherConfig
    {
        public ulong SendHighWaterMark { get; set; }

        public TimeSpan? SendTimeout { get; set; }

        public TimeSpan? Linger { get; set; }
    }

    internal sealed class SpotSubscriberConfig : IZLinkSpotSubscriberConfig
    {
        public ulong ReceiveHighWaterMark { get; set; }

        public TimeSpan? ReceiveTimeout { get; set; }

        public TimeSpan? Linger { get; set; }
    }

    internal sealed class DispatchOptions : IZLinkDispatchOptions
    {
        public IZLinkUnhandledDispatchOptions Unhandled { get; } = new UnhandledDispatchOptions();

        public IZLinkDiagnosticsOptions Diagnostics { get; } = new DiagnosticsOptions();
    }

    private sealed class UnhandledDispatchOptions : IZLinkUnhandledDispatchOptions
    {
        public ZLinkUnhandledDispatchAction Request { get; set; }

        public ZLinkUnhandledDispatchAction Send { get; set; }

        public ZLinkUnhandledDispatchAction Publish { get; set; }
    }

    private sealed class DiagnosticsOptions : IZLinkDiagnosticsOptions
    {
        public IZLinkDiagnosticsOptions SetLevel(ZLinkDiagnosticsLevel level) => this;

        public IZLinkDiagnosticsOptions SetSampleRate(double rate) => this;

        public IZLinkDiagnosticsOptions IncludeMessageSizes(bool include) => this;
    }
}
