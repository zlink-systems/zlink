using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.UnitTests;

public sealed class MonitoringTests : RegistrationValidationSupport
{
    [Fact]
    public void RemovedSpotEgressClient_DI_IsNotExposed_AsPublicCapability()
    {
        var withoutEgress = new ServiceCollection();
        withoutEgress.AddZLinkFramework(options =>
        {
            var mesh = options.AddRouteMesh("gateway")
                .Listen("tcp://127.0.0.1:7101")
                .SetRoutingId(RoutingId.From("gateway"));
            mesh.Channel("gateway").Server().SetWeight(0);
        });

        using (var provider = withoutEgress.BuildServiceProvider())
        {
            _ = provider;
            Assert.DoesNotContain(typeof(IZLinkSpotOutbound).Assembly.GetTypes(), IsRemovedSpotEgressClient);
        }

        var routeMeshEgress = new ServiceCollection();
        routeMeshEgress.AddZLinkFramework(options =>
        {
            var mesh = options.AddRouteMesh("gateway.route")
                .Listen("tcp://127.0.0.1:7301")
                .SetRoutingId(RoutingId.From("gateway-route"));
            mesh.Channel("gateway.route").Server();
            mesh.PeerConnections.Connect("tcp://127.0.0.1:7201");
        });

        using (var provider = routeMeshEgress.BuildServiceProvider())
        {
            _ = provider;
            Assert.DoesNotContain(typeof(IZLinkSpotOutbound).Assembly.GetTypes(), IsRemovedSpotEgressClient);
        }
    }

    private static bool IsRemovedSpotEgressClient(Type type)
    {
        return type.Namespace == "Zlink.Framework.Contracts.Spots"
               && type.Name.Contains("Routed", StringComparison.Ordinal)
               && type.Name.Contains("Spot", StringComparison.Ordinal)
               && type.Name.Contains("Client", StringComparison.Ordinal);
    }

    [Fact]
    public void AddZLinkFramework_Throws_WhenPublisherHasNoBindEndpoint()
    {
        var services = new ServiceCollection();

        var exception = Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options => { options.AddFanoutChannel("profile").EnablePublisher(""); }));

        Assert.Contains("Channel publisher bind endpoint must not be empty", exception.Message,
            StringComparison.Ordinal);
    }

    [Fact]
    public void AddZLinkFramework_RegistersValidatedConfigurationAndFilterTypes()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.DefaultRequestTimeout = TimeSpan.FromSeconds(5);
            options.Codecs.Use(ZLinkProtobufCodec.Default);
            options.UseFilter<TestFilter>();
            options.UseTestLocationStore();

            var profile = options.AddRouteMesh("profile")
                .Listen("tcp://127.0.0.1:7101")
                .SetRoutingIdPrefix("profile");
            profile.Channel("profile").Server()
                .AddRequestHandler<TestChannelRequestHandler, TestChannelRequest, TestChannelReply>();

            {
                var events = options.AddFanoutChannel("profile.events");
                events.Connect("inproc://profile-events");
                events.AddHandler<TestPublishHandler, TestPublishedEvent>();
            }

            {
                var stream = options.AddStreamNode("stream.node");
                stream.Bind("tcp://127.0.0.1:9100");
                stream.AddSession<TestHeaderSession>();
            }

            {
                var mesh = options.AddRouteMesh("game.stage");
                mesh.Channel("game.stage").Server();
                {
                    var spot = mesh;
                    {
                        var router = spot.Listen("tcp://127.0.0.1:9000");
                    }
                    spot.Objects().Server().AddSpotFactory<TestSpot>(
                        "test", factory => factory.DisableRelocation());
                }
            }
        });

        using var provider = services.BuildServiceProvider();
        var registration = provider.GetRequiredService<ZLinkFrameworkRegistration>();
        var filter = provider.GetRequiredService<TestFilter>();

        Assert.NotNull(filter);
        Assert.Equal(TimeSpan.FromSeconds(5), registration.DefaultRequestTimeout);
        Assert.Equal(TimeSpan.FromMilliseconds(1000), registration.DefaultSocketSendTimeout);
        Assert.Contains("application/x-protobuf", registration.Codecs.Serializers.Keys);
        Assert.True(registration.Locations.Enabled);
        Assert.Contains("profile", registration.SpotNodes.Keys);
        Assert.Contains("stream.node", registration.StreamNodes.Keys);
        Assert.Contains("game.stage", registration.SpotNodes.Keys);
    }

    [Fact]
    public void MeshDefaultRequestTimeoutOverridesGlobalDefault()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.DefaultRequestTimeout = TimeSpan.FromSeconds(30);
            var mesh = options.AddRouteMesh("route")
                .Listen("tcp://127.0.0.1:7201")
                .SetRoutingId(RoutingId.From("route"))
                .SetDefaultRequestTimeout(TimeSpan.FromSeconds(3));
            mesh.Channel("api").Server();
        });

        using var provider = services.BuildServiceProvider();
        var registration = provider.GetRequiredService<ZLinkFrameworkRegistration>();

        Assert.Equal(
            TimeSpan.FromSeconds(3),
            registration.SpotNodes["route"].DefaultRequestTimeout);
        Assert.Equal(TimeSpan.FromSeconds(30), registration.ResolveChannelRequestTimeout("missing"));
        Assert.Equal(TimeSpan.FromSeconds(3), registration.ResolveMeshRequestTimeout("route"));
        Assert.Equal(TimeSpan.FromSeconds(30), registration.ResolveMeshRequestTimeout("missing"));
    }

    private sealed class FailIfRuntimeStartsBackendAdapterFactory : IZLinkBackendAdapterFactory
    {
        public IZLinkChannelBackendAdapter CreateChannelAdapter() =>
            throw new InvalidOperationException("Static monitoring validation must run before native startup.");

        public IZLinkSpotBackendAdapter CreateSpotAdapter() =>
            throw new InvalidOperationException("Static monitoring validation must run before native startup.");

        public IZLinkStreamBackendAdapter CreateStreamAdapter() =>
            throw new InvalidOperationException("Static monitoring validation must run before native startup.");

        public IZLinkMonitoringBackendAdapter CreateMonitoringAdapter() => new UnusedMonitoringBackendAdapter();
    }

    private sealed class UnusedMonitoringBackendAdapter : IZLinkMonitoringBackendAdapter
    {
        public IZLinkBackendSocketMonitor OpenSocketMonitor(IZLinkBackendSocket socket) =>
            throw new InvalidOperationException("Static monitoring validation must not open a monitor.");
    }
}
