using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.AspNetCore;

namespace Zlink.Framework.UnitTests;

public sealed class ChannelsTests : RegistrationValidationSupport
{
    [Fact]
    public void AddZLinkFramework_Throws_WhenMeshNameIsDuplicated()
    {
        var services = new ServiceCollection();

        var exception = Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
            {
                options.AddRouteMesh("profile");
                options.AddRouteMesh("profile");
            }));

        Assert.Contains("Duplicate RouteMesh name", exception.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void RemovedTopologyRegistrationMethods_AreAbsent()
    {
        Assert.Null(typeof(IZLinkFrameworkOptions).GetMethod("AddChannel"));
        Assert.Null(typeof(IZLinkFrameworkOptions).GetMethod("AddRouteChannel"));
    }

    [Fact]
    public void MeshPeerConnections_RecordManualPeers()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            var mesh = options.AddRouteMesh("play")
                .Listen("tcp://127.0.0.1:7101")
                .SetRoutingId(RoutingId.From("play"));
            mesh.Channel("play").Server();
            mesh.PeerConnections.Connect(
                RoutingId.From("peer"), "tcp://127.0.0.1:7102");
        });

        var registration = services.BuildServiceProvider()
            .GetRequiredService<ZLinkFrameworkRegistration>();
        var node = Assert.Single(registration.SpotNodes.Values);
        var endpoint = Assert.Single(node.Router!.ManualConnections.ListConnections());
        Assert.Equal("tcp://127.0.0.1:7102", endpoint);
        Assert.Equal(RoutingId.From("peer"), node.Router.PeerRoutingIds[endpoint]);
    }

    [Fact]
    public void RouteMeshChannelRoles_SeparateClientFromPublishedServerMembership()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            var mesh = options.AddRouteMesh("play")
                .Listen("tcp://127.0.0.1:7101");
            mesh.Channel("outbound-only").Client();
            mesh.Channel("requests")
                .Server()
                .SetWeight(300)
                .AddRequestHandler<
                    TestChannelRequestHandler,
                    TestChannelRequest,
                    TestChannelReply>();
        });

        var registration = services.BuildServiceProvider()
            .GetRequiredService<ZLinkFrameworkRegistration>();
        var memberships = Assert.Single(registration.SpotNodes.Values)
            .ChannelMemberships
            .ToDictionary(static membership => membership.ChannelName);

        Assert.False(memberships["outbound-only"].IsServer);
        Assert.Empty(memberships["outbound-only"].RequestHandlers);
        Assert.True(memberships["requests"].IsServer);
        Assert.Equal(300, memberships["requests"].Weight);
        Assert.Single(memberships["requests"].RequestHandlers);
    }

    [Fact]
    public void RouteMeshChannelRole_RejectsDuplicateChannelName()
    {
        var services = new ServiceCollection();

        var exception = Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
            {
                var mesh = options.AddRouteMesh("play")
                    .Listen("tcp://127.0.0.1:7101");
                mesh.Channel("requests").Client();
                mesh.Channel("requests").Server();
            }));

        Assert.Contains("Duplicate channel membership", exception.Message);
    }

    [Theory]
    [InlineData("")]
    [InlineData("contains space")]
    [InlineData("한글")]
    [InlineData("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")]
    public void MeshNodeRoutingIdPrefix_RejectsNonCanonicalValues(string prefix)
    {
        var services = new ServiceCollection();

        Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
                options.AddRouteMesh("play").SetRoutingIdPrefix(prefix)));
    }

    [Fact]
    public void MeshNodeRoutingMode_CanBeConfiguredOnlyOnce()
    {
        var services = new ServiceCollection();

        Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
            {
                var mesh = options.AddRouteMesh("play");
                mesh.SetRoutingIdPrefix("play");
                mesh.SetRoutingId(RoutingId.From("fixed"));
            }));
    }

    [Fact]
    public void AutomaticRouteMesh_RejectsFixedRoutingId()
    {
        var services = new ServiceCollection();

        var error = Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
            {
                options.UseTestLocationStore();
                options.AddRouteMesh("play")
                    .Listen("tcp://127.0.0.1:7101")
                    .SetRoutingId(RoutingId.From("fixed"));
            }));

        Assert.Contains("fixed routing ID", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ManualRouteMesh_RejectsRoutingIdPrefix()
    {
        var services = new ServiceCollection();

        var error = Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
            {
                var mesh = options.AddRouteMesh("play")
                    .Listen("tcp://127.0.0.1:7101")
                    .SetRoutingIdPrefix("play");
                mesh.PeerConnections.Connect(
                    RoutingId.From("peer"),
                    "tcp://127.0.0.1:7102");
            }));

        Assert.Contains("only with automatic discovery", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void MeshNodeCapacitySetters_RecordIndependentActorSpotAndActivationLimits()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
            options.AddRouteMesh("play")
                .Listen("tcp://127.0.0.1:7101")
                .SetActorLimit(101)
                .SetSpotLimit(202)
                .SetActivationConcurrency(17));

        var node = Assert.Single(services.BuildServiceProvider()
            .GetRequiredService<ZLinkFrameworkRegistration>()
            .SpotNodes.Values);
        Assert.Equal(101, node.ActorLimit);
        Assert.Equal(202, node.SpotLimit);
        Assert.Equal(17, node.ActivationConcurrencyLimit);
    }

    [Fact]
    public void MeshNodeInstanceSpotIdleTimeout_UsesZeroAsDisabledAndStoresValue()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
            options.AddRouteMesh("play")
                .Listen("tcp://127.0.0.1:7101")
                .SetInstanceSpotIdleTimeout(TimeSpan.FromSeconds(3)));

        var node = Assert.Single(services.BuildServiceProvider()
            .GetRequiredService<ZLinkFrameworkRegistration>()
            .SpotNodes.Values);
        Assert.Equal(TimeSpan.FromSeconds(3), node.InstanceSpotIdleTimeout);
    }

    [Fact]
    public void MeshNodeInstanceSpotIdleTimeout_RejectsNegativeValue()
    {
        var services = new ServiceCollection();

        Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
                options.AddRouteMesh("play")
                    .SetInstanceSpotIdleTimeout(TimeSpan.FromMilliseconds(-1))));
    }

    [Fact]
    public void MeshNodeCapacitySetters_AllowUnlimitedPopulationLimits()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
            options.AddRouteMesh("play")
                .Listen("tcp://127.0.0.1:7101")
                .SetActorLimit(0)
                .SetSpotLimit(0));

        var node = Assert.Single(services.BuildServiceProvider()
            .GetRequiredService<ZLinkFrameworkRegistration>()
            .SpotNodes.Values);
        Assert.Equal(0, node.ActorLimit);
        Assert.Equal(0, node.SpotLimit);
    }

    [Fact]
    public void ConfigureDispatch_RejectsInvalidPolicies()
    {
        var services = new ServiceCollection();

        var send = Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
                options.ConfigureDispatch().Unhandled.Send = ZLinkUnhandledDispatchAction.ReplyError));
        Assert.Contains("send dispatch cannot use ReplyError", send.Message, StringComparison.Ordinal);

        var sampleRate = Assert.Throws<ArgumentOutOfRangeException>(() =>
            services.AddZLinkFramework(options =>
                options.ConfigureDispatch().Diagnostics.SetSampleRate(1.1d)));
        Assert.Equal("rate", sampleRate.ParamName);
    }
}
