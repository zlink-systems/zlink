using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using System.Reflection;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Handlers;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests;

public sealed class NodesAndServicesTests : RegistrationValidationSupport
{
    [Fact]
    public void AddZLinkFramework_Registers_Internal_RemoteSessionRouteHandlers()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(_ => { });

        Assert.Contains(
            services,
            descriptor => descriptor.ServiceType == typeof(ZLinkRemoteSessionBindRouteHandler));
        Assert.Contains(
            services,
            descriptor => descriptor.ServiceType == typeof(ZLinkRemoteSessionUnbindRouteHandler));
        Assert.Contains(
            services,
            descriptor => descriptor.ServiceType == typeof(ZLinkSessionRouteSealHandler));
        Assert.Contains(
            services,
            descriptor => descriptor.ServiceType == typeof(ZLinkSessionRouteAbortHandler));
        Assert.Contains(
            services,
            descriptor => descriptor.ServiceType == typeof(ZLinkSessionRouteCommitHandler));
        Assert.Contains(
            services,
            descriptor => descriptor.ServiceType == typeof(ZLinkSessionRouteUnsealHandler));
    }

    [Fact]
    public void AddZLinkFramework_Rejects_Duplicate_Registration()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(_ => { });

        var error = Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(_ => { }));

        Assert.Contains("already configured", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void FactoryBuildersRequireExactlyOneRelocationChoice()
    {
        var missingActor = Assert.Throws<ZLinkConfigurationException>(() =>
            new ServiceCollection().AddZLinkFramework(options =>
                options.AddRouteMesh("actor-node")
                    .Objects()
                    .Server()
                    .AddActorFactory<TestActor, TestActorFactory>(
                        "actor",
                        _ => { })));
        Assert.Contains("exactly one relocation policy", missingActor.Message);

        var missingSpot = Assert.Throws<ZLinkConfigurationException>(() =>
            new ServiceCollection().AddZLinkFramework(options =>
                options.AddRouteMesh("spot-node")
                    .Objects()
                    .Server()
                    .AddSpotFactory<TestSpot>(
                        "spot",
                        _ => { })));
        Assert.Contains("exactly one relocation policy", missingSpot.Message);

        var missingInstance = Assert.Throws<ZLinkConfigurationException>(() =>
            new ServiceCollection().AddZLinkFramework(options =>
                options.AddRouteMesh("instance-node")
                    .Objects()
                    .Server()
                    .AddInstanceSpotFactory<TestInstanceSpot>(
                        "instance",
                        _ => { })));
        Assert.Contains("exactly one relocation policy", missingInstance.Message);

        var duplicate = Assert.Throws<ZLinkConfigurationException>(() =>
            new ServiceCollection().AddZLinkFramework(options =>
                options.AddRouteMesh("duplicate-node")
                    .Objects()
                    .Server()
                    .AddSpotFactory<TestSpot>(
                        "spot",
                        factory => factory
                            .DisableRelocation()
                            .RecreateOnRelocation())));
        Assert.Contains("exactly one relocation policy", duplicate.Message);

        IZLinkUserSpotFactoryBuilder<TestSpot>? escaped = null;
        new ServiceCollection().AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            options.AddRouteMesh("sealed-node")
                .Listen("inproc://sealed-node")
                .Objects()
                .Server()
                .AddSpotFactory<TestSpot>(
                    "spot",
                    factory =>
                    {
                        escaped = factory;
                        factory.DisableRelocation();
                    });
        });
        Assert.Throws<ZLinkConfigurationException>(
            () => escaped!.StableTypeLimit(8));

        Assert.Throws<ZLinkConfigurationException>(() =>
            new ServiceCollection().AddZLinkFramework(options =>
                options.AddRouteMesh("zero-limit-node")
                    .Objects()
                    .Server()
                    .AddSpotFactory<TestSpot>(
                        "spot",
                        factory => factory
                            .StableTypeLimit(0)
                            .DisableRelocation())));

        var callbackFailure = new InvalidOperationException("configure failed");
        var propagated = Assert.Throws<InvalidOperationException>(() =>
            new ServiceCollection().AddZLinkFramework(options =>
                options.AddRouteMesh("failed-node")
                    .Objects()
                    .Server()
                    .AddSpotFactory<TestSpot>(
                        "spot",
                        _ => throw callbackFailure)));
        Assert.Same(callbackFailure, propagated);
    }

    [Fact]
    public void AddZLinkFramework_Rejects_TheSameEntrySpotType_AcrossNodes()
    {
        var services = new ServiceCollection();

        var exception = Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
            {
                options.UseTestLocationStore();
                var first = options.AddRouteMesh("entry-a").Listen("inproc://entry-a");
                first.Channel("entry-a").Server();
                first.Objects().Server().AddEntrySpot<TestEntrySpot>();
                var second = options.AddRouteMesh("entry-b").Listen("inproc://entry-b");
                second.Channel("entry-b").Server();
                second.Objects().Server().AddEntrySpot<TestEntrySpot>();
            }));

        Assert.Contains("Duplicate Entry Spot", exception.Message, StringComparison.Ordinal);
        Assert.Contains(typeof(TestEntrySpot).ToString(), exception.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AddZLinkFramework_Throws_WhenStreamNodeRegistersMultipleSessions()
    {
        var services = new ServiceCollection();

        var exception = Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
            {
                {
                    var stream = options.AddStreamNode("client.stream");
                    stream.Bind("tcp://127.0.0.1:9100");
                    stream.AddSession<TestHeaderSession>();
                    stream.AddSession<TestHeaderSession>();
                }
            }));

        Assert.Contains("already has a stream session", exception.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AddZLinkFramework_AllowsStandaloneLocalSpotNode()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            {
                var mesh = options.AddRouteMesh("stage-node");
                mesh.Channel("stage-node").Server();
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

        var registration = services.BuildServiceProvider().GetRequiredService<ZLinkFrameworkRegistration>();
        Assert.Single(registration.SpotNodes);
    }

    [Fact]
    public void AddZLinkFramework_AddRouteMesh_CanConfigureDefaultSpotNode()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            var node = options.AddRouteMesh("stage-node")
                .Listen("tcp://127.0.0.1:9000");
            node.Channel("stage-node").Server();
            node.Objects().Server().AddSpotFactory<TestSpot>(
                "test", factory => factory.DisableRelocation());
        });

        var registration = services.BuildServiceProvider().GetRequiredService<ZLinkFrameworkRegistration>();
        var node = Assert.Single(registration.SpotNodes.Values);
        Assert.Equal("stage-node", node.SpotNodeName);
        Assert.NotNull(node.Router);
        Assert.Contains(typeof(TestSpot), node.SpotFactories);
    }

    [Fact]
    public void AddZLinkFramework_AddRouteMesh_AllowsMultipleProcessLocalNodes()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            var play = options.AddRouteMesh("play-node")
                .Listen("tcp://127.0.0.1:9001");
            play.Channel("play-node").Server();
            play.Objects().Server().AddSpotFactory<TestSpot>(
                "play", factory => factory.DisableRelocation());
            var session = options.AddRouteMesh("session-node")
                .Listen("tcp://127.0.0.1:9002");
            session.Channel("session-node").Server();
            session.Objects().Server().AddSpotFactory<OtherTestSpot>(
                "session", factory => factory.DisableRelocation());
        });

        var registration = services.BuildServiceProvider().GetRequiredService<ZLinkFrameworkRegistration>();
        Assert.Equal(new[] { "play-node", "session-node" }, registration.SpotNodes.Keys);
        Assert.Equal("play-node", registration.SpotNodes["play-node"].SpotMeshChannelName);
        Assert.Equal("session-node", registration.SpotNodes["session-node"].SpotMeshChannelName);
    }

    [Fact]
    public void AddZLinkFramework_Throws_WhenSpotMeshNameIsDuplicated()
    {
        var services = new ServiceCollection();

        var exception = Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
            {
                options.UseTestLocationStore();
                options.AddRouteMesh("play-node").Listen("tcp://127.0.0.1:9001").Channel("play-node").Server();
                options.AddRouteMesh("play-node").Listen("tcp://127.0.0.1:9002").Channel("play-node").Server();
            }));

        Assert.Contains("Duplicate RouteMesh name 'play-node'", exception.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AddZLinkFramework_Throws_WhenSpotFactoryTypeIsDuplicatedAcrossNodes()
    {
        var services = new ServiceCollection();

        var exception = Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
            {
                options.UseTestLocationStore();
                var first = options.AddRouteMesh("game.stage-a")
                    .Listen("tcp://127.0.0.1:6101");
                first.Channel("game.stage").Server();
                first.Objects().Server().AddSpotFactory<TestSpot>(
                    "test-a", factory => factory.DisableRelocation());

                var second = options.AddRouteMesh("game.stage-b")
                    .Listen("tcp://127.0.0.1:6102");
                second.Channel("game.stage").Server();
                second.Objects().Server().AddSpotFactory<TestSpot>(
                    "test-b", factory => factory.DisableRelocation());
            }));

        Assert.Contains("Duplicate SPOT factory", exception.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AddZLinkFramework_Throws_WhenSpotNodeRegistersMultipleEntrySpots()
    {
        var services = new ServiceCollection();

        var exception = Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
            {
                options.UseTestLocationStore();
                {
                    var mesh = options.AddRouteMesh("game.stage");
                    mesh.Channel("game.stage").Server();
                    {
                        var spot = mesh;
                        {
                            var router = spot.Listen("tcp://127.0.0.1:6101");
                        }
                        var server = spot.Objects().Server();
                        server.AddEntrySpot<TestEntrySpot>();
                        server.AddEntrySpot<TestEntrySpot>();
                    }
                }
            }));

        Assert.Contains("Duplicate Entry Spot registry", exception.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AddZLinkFramework_Throws_WhenActorFactoryNameIsDuplicated()
    {
        var services = new ServiceCollection();

        var exception = Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
            {
                options.UseTestLocationStore();
                var node = options.AddRouteMesh("actor-node")
                    .Listen("tcp://127.0.0.1:6102");
                node.Channel("actor-node").Server();
                var server = node.Objects().Server();
                server.AddActorFactory<TestActor, TestActorFactory>(
                    "warrior", factory => factory.DisableRelocation());
                server.AddActorFactory<TestActor, TestActorFactory>(
                    "warrior", factory => factory.DisableRelocation());
            }));

        Assert.Contains("Duplicate actor factory 'warrior'", exception.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AddZLinkFramework_Throws_WhenActorFactoryNodeHasNoRouterCapability()
    {
        var services = new ServiceCollection();

        var exception = Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
            {
                options.UseTestLocationStore();
                options.AddRouteMesh("actor-node").Objects().Server()
                    .AddActorFactory<TestActor, TestActorFactory>(
                        "warrior", factory => factory.DisableRelocation());
            }));

        Assert.Contains("ROUTER endpoint", exception.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AddZLinkFramework_Allows_Multiple_SpotNodes_To_Own_ActorFactories()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            var first = options.AddRouteMesh("actor-node-a")
                .Listen("tcp://127.0.0.1:6103");
            first.Channel("actor-node-a").Server();
            first.Objects().Server().AddActorFactory<TestActor, TestActorFactory>(
                "warrior", factory => factory.DisableRelocation());
            var second = options.AddRouteMesh("actor-node-b")
                .Listen("tcp://127.0.0.1:6104");
            second.Channel("actor-node-b").Server();
            second.Objects().Server().AddActorFactory<TestActor, TestActorFactory>(
                "mage", factory => factory.DisableRelocation());
        });

        using var provider = services.BuildServiceProvider();
        Assert.NotNull(provider.GetRequiredService<ZLinkFrameworkRegistration>());
    }

    [Fact]
    public void AddZLinkFramework_DoesNot_Register_ActorManager_Without_SpotNode()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(_ => { });

        using var provider = services.BuildServiceProvider();
        Assert.Null(provider.GetService<IZLinkActorManager>());
    }

    [Fact]
    public void AddZLinkFramework_DoesNot_Register_SpotServices_Without_SpotNode()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(_ => { });

        using var provider = services.BuildServiceProvider();
        Assert.Null(provider.GetService<IZLinkSpotManager>());
        Assert.Null(provider.GetService<IZLinkSpotOutbound>());
        Assert.Null(provider.GetService<IZLinkSpotPublisherClient>());
    }

    [Fact]
    public async Task AddZLinkFramework_Registers_SpotServices_When_SpotNode_Exists()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            {
                var mesh = options.AddRouteMesh("stage-node");
                mesh.Channel("stage-node").Server();
                {
                    var spot = mesh;
                    {
                        var router = spot.Listen("tcp://127.0.0.1:6200");
                    }
                }
            }
        });

        await using var provider = services.BuildServiceProvider();
        Assert.NotNull(provider.GetService<IZLinkSpotManager>());
        Assert.NotNull(provider.GetService<IZLinkSpotOutbound>());
    }

    [Fact]
    public void AddZLinkFramework_DoesNot_Register_ActorManager_With_SpotNode_Only()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            {
                var mesh = options.AddRouteMesh("stage-node");
                mesh.Channel("stage-node").Server();
                {
                    var spot = mesh;
                    {
                        var router = spot.Listen("tcp://127.0.0.1:6203");
                    }
                }
            }
        });

        using var provider = services.BuildServiceProvider();
        Assert.Null(provider.GetService<IZLinkActorManager>());
    }

    [Fact]
    public async Task AddZLinkFramework_Registers_ActorManager_When_SpotNode_And_ActorFactory_Exist()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            options.AddRelocationStore(new TestRelocationStore());
            var node = options.AddRouteMesh("actor-node")
                .Listen("tcp://127.0.0.1:6201");
            node.Channel("actor-node").Server();
            node.Objects().Server().AddActorFactory<TestActor, TestActorFactory>(
                "warrior", factory => factory.DisableRelocation());
        });

        await using var provider = services.BuildServiceProvider();
        Assert.NotNull(provider.GetService<IZLinkActorManager>());
    }

    [Fact]
    public void SnapshotRelocation_Registers_Adapter_As_Scoped_Service()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            options.AddRelocationStore(new TestRelocationStore());
            var node = options.AddRouteMesh("actor-node")
                .Listen("tcp://127.0.0.1:6202");
            node.Channel("actor-node").Server();
            node.Objects().Server().AddActorFactory<TestActor, TestActorFactory>(
                "warrior", factory => factory.PreserveStateWith<TestActorRelocationAdapter>());
        });

        using var provider = services.BuildServiceProvider();
        using var scope = provider.CreateScope();
        Assert.NotNull(scope.ServiceProvider.GetService<TestActorRelocationAdapter>());
    }

    [Fact]
    public void AddZLinkFramework_AllowsActorTypeOnMultipleEligibleNodes()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            options.AddRelocationStore(new TestRelocationStore());
            var first = options.AddRouteMesh("actor-node-a")
                .Listen("tcp://127.0.0.1:6201");
            first.Channel("actor-node-a").Server();
            first.Objects().Server().AddActorFactory<TestActor, TestActorFactory>(
                "warrior", factory => factory.PreserveStateWith<TestActorRelocationAdapter>());
            var second = options.AddRouteMesh("actor-node-b")
                .Listen("tcp://127.0.0.1:6202");
            second.Channel("actor-node-b").Server();
            second.Objects().Server().AddActorFactory<TestActor, TestActorFactory>(
                "warrior", factory => factory.PreserveStateWith<TestActorRelocationAdapter>());
        });

        using var provider = services.BuildServiceProvider();
        Assert.NotNull(provider.GetRequiredService<ZLinkFrameworkRegistration>());
    }

    [Fact]
    public void AddZLinkFramework_RegistersSnapshotPolicyWithItsActorFactory()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            options.AddRelocationStore(new TestRelocationStore());
            var node = options.AddRouteMesh("actor-node-a")
                .Listen("tcp://127.0.0.1:6203");
            node.Channel("actor-node-a").Server();
            node.Objects().Server().AddActorFactory<TestActor, TestActorFactory>(
                "warrior", factory => factory.PreserveStateWith<TestActorRelocationAdapter>());
        });

        var registration = services.BuildServiceProvider()
            .GetRequiredService<ZLinkFrameworkRegistration>();
        Assert.True(registration.SpotNodes["actor-node-a"].ActorRelocations.ContainsKey("warrior"));
    }

    [Fact]
    public void SessionActorDispatch_Allows_Multiple_Object_Meshes_Without_Inferred_Relay_Target()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            {
                var mesh = options.AddRouteMesh("actor-node");
                mesh.Channel("actor-node").Server();
                mesh.Objects().Server();
                {
                    var spot = mesh;
                    {
                        var router = spot.Listen("tcp://127.0.0.1:7302");
                    }
                }
            }
            {
                var mesh = options.AddRouteMesh("actor-node-2");
                mesh.Channel("actor-node-2").Server();
                mesh.Objects().Server();
                mesh.Listen("tcp://127.0.0.1:7303");
            }
            {
                var stream = options.AddStreamNode("stream.node");
                stream.Bind("tcp://127.0.0.1:9100");
                stream.EnableActorDispatch();
                stream.AddSession<TestHeaderSession>();
            }
        });

        using var provider = services.BuildServiceProvider();
        var registration = provider.GetRequiredService<ZLinkFrameworkRegistration>();

        Assert.Single(registration.StreamNodes);
        Assert.Equal(2, registration.SpotNodes.Count);
        Assert.True(registration.StreamNodes["stream.node"].ActorDispatchEnabled);
    }

    [Fact]
    public void AddZLinkFramework_Allows_ObjectClient_With_ChannelServer()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            var node = options.AddRouteMesh("client-node")
                .Listen("inproc://client-node");
            node.Objects().Client();
            node.Channel("orders").Server().SetWeight(0);
        });

        using var provider = services.BuildServiceProvider();
        var registration = provider.GetRequiredService<ZLinkFrameworkRegistration>();
        var nodeRegistration = Assert.Single(registration.SpotNodes.Values);
        Assert.Equal(ZLinkMeshNodeObjectRole.Client, nodeRegistration.ObjectRole);
        var channel = Assert.Single(nodeRegistration.ChannelMemberships);
        Assert.True(channel.IsServer);
        Assert.Equal(0, channel.Weight);
    }

    [Fact]
    public void AddZLinkFramework_Rejects_ObjectClient_With_NodeDirectHandler()
    {
        var services = new ServiceCollection();

        var exception = Assert.Throws<ZLinkConfigurationException>(() =>
            services.AddZLinkFramework(options =>
            {
                options.UseTestLocationStore();
                var node = options.AddRouteMesh("client-node")
                    .Listen("inproc://client-node");
                node.Objects().Client();
                node.AddRouteRequestHandler<
                    TestRouteRequestHandler,
                    TestRouteRequest,
                    TestRouteReply>();
            }));

        Assert.Contains("Object Client", exception.Message, StringComparison.Ordinal);
        Assert.Contains("Node-direct handlers", exception.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AddZLinkFramework_Uses_Standard_DI_For_Application_Dependencies()
    {
        var services = new ServiceCollection();
        services.AddScoped<ITestSessionDependencyHandler, TestSessionDependencyHandler>();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            {
                var stream = options.AddStreamNode("client.stream");
                stream.Bind("tcp://127.0.0.1:9100");
                stream.AddSession<TestSessionWithEnumerableHandlers>();
            }
            {
                var mesh = options.AddRouteMesh("stage-node");
                mesh.Channel("stage-node").Server();
                {
                    var spot = mesh;
                    {
                        var router = spot.Listen("tcp://127.0.0.1:6204");
                    }
                    var server = spot.Objects().Server();
                    server.AddSpotFactory<TestSpot>(
                        "test", factory => factory.DisableRelocation());
                    server.AddEntrySpot<TestEntrySpot>();
                }
            }
        });

        Assert.DoesNotContain(services,
            static service => service.ServiceType == typeof(TestSessionWithEnumerableHandlers));
        Assert.Contains(services, static service => service.ServiceType == typeof(TestSpot));
        Assert.Contains(services, static service => service.ServiceType == typeof(TestEntrySpot));

        using var provider = services.BuildServiceProvider();
        Assert.IsType<TestSessionDependencyHandler>(
            Assert.Single(provider.GetServices<ITestSessionDependencyHandler>()));
    }

    [Fact]
    public async Task SessionHandlerRegistry_Handles_ManuallyRegistered_Typed_Handler()
    {
        var services = new ServiceCollection();
        services.AddScoped<TestSessionPacketHandler>();

        using var provider = services.BuildServiceProvider();
        await using var scope = provider.CreateAsyncScope();
        await using var handlerInstances = new ZLinkScopedHandlerInstanceOwner(scope.ServiceProvider);
        var context = new TestSessionPacketContext();
        var registry = new ZLinkSessionHandlerRegistry(handlerInstances);
        registry.BindContext(context);
        registry.AddHandler<TestSessionPacketHandler>();
        registry.Bind();

        var handled = await registry.TryHandleAsync(
            new ZLinkSessionDispatchContext(nameof(TestSessionPacketMessage)),
            ZLinkMessage.From(new TestSessionPacketMessage()));
        var unhandled = await registry.TryHandleAsync(
            new ZLinkSessionDispatchContext("test.unhandled"),
            ZLinkMessage.From(new TestSessionPacketMessage()));

        Assert.True(handled);
        Assert.False(unhandled);
        Assert.Equal(1, context.HandledCount);
    }

    [Fact]
    public async Task SessionHandlerRegistry_Reuses_Unregistered_Handler_And_Disposes_It_On_Disconnect()
    {
        var lifetime = new AsyncSessionHandlerLifetime();
        using var provider = new ServiceCollection()
            .AddSingleton(lifetime)
            .BuildServiceProvider();
        var handlerInstances = new ZLinkScopedHandlerInstanceOwner(provider);
        var context = new TestSessionPacketContext();
        var registry = new ZLinkSessionHandlerRegistry(handlerInstances);
        registry.BindContext(context);
        registry.AddHandler<AsyncDisposableSessionPacketHandler>();
        registry.Bind();

        await registry.TryHandleAsync(
            new ZLinkSessionDispatchContext(nameof(AsyncDisposableSessionPacketMessage)),
            ZLinkMessage.From(new AsyncDisposableSessionPacketMessage()));
        await registry.TryHandleAsync(
            new ZLinkSessionDispatchContext(nameof(AsyncDisposableSessionPacketMessage)),
            ZLinkMessage.From(new AsyncDisposableSessionPacketMessage()));

        Assert.Equal(2, lifetime.Invocations.Count);
        Assert.Same(lifetime.Invocations[0], lifetime.Invocations[1]);
        await handlerInstances.DisposeAsync();
        await handlerInstances.DisposeAsync();
        Assert.Equal(1, lifetime.DisposeCount);
    }

    [Fact]
    public async Task SessionHandlerRegistry_AutoRegisters_Compatible_Typed_Handlers_From_Assembly()
    {
        using var provider = new ServiceCollection().BuildServiceProvider();
        await using var handlerInstances = new ZLinkScopedHandlerInstanceOwner(provider);
        var context = new TestSessionPacketContext();
        var registry = new ZLinkSessionHandlerRegistry(handlerInstances);
        registry.BindContext(context);
        registry.AddScannedHandlers(ZLinkScannedSessionHandlerScanner.Scan(
            typeof(TestSessionPacketHandler).Assembly,
            new HashSet<Type>()));
        registry.Bind();

        var handled = await registry.TryHandleAsync(
            new ZLinkSessionDispatchContext(nameof(TestSessionPacketMessage)),
            ZLinkMessage.From(new TestSessionPacketMessage()));

        Assert.True(handled);
        Assert.Equal(1, context.HandledCount);
    }

    [Fact]
    public async Task SessionHandlerRegistry_Invokes_Attributed_Method_On_Active_Session()
    {
        using var provider = new ServiceCollection().BuildServiceProvider();
        await using var handlerInstances = new ZLinkScopedHandlerInstanceOwner(provider);
        var context = new TestSessionPacketContext();
        var session = new TestHeaderSession(context);
        var registry = new ZLinkSessionHandlerRegistry(handlerInstances);
        registry.BindContext(context);
        registry.BindSession(session);
        registry.AddScannedHandlers(ZLinkScannedSessionHandlerScanner.Scan(
            typeof(TestHeaderSession).Assembly,
            new HashSet<Type> { typeof(TestHeaderSession) }));
        registry.Bind();

        var handled = await registry.TryHandleAsync(
            new ZLinkSessionDispatchContext(nameof(AttributedSessionPacketMessage)),
            ZLinkMessage.From(new AttributedSessionPacketMessage()));

        Assert.True(handled);
        Assert.Equal(1, session.AttributedHandledCount);
    }

    [Fact]
    public void Registration_Includes_Registered_Application_Assemblies_In_Handler_Scan_By_Default()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            options.AddStreamNode("client.stream")
                .Bind("tcp://127.0.0.1:9100")
                .AddSession<TestHeaderSession>();
            var node = options.AddRouteMesh("stage-node")
                .Listen("tcp://127.0.0.1:9000");
            node.Channel("stage-node").Server();
            node.Objects().Server()
                .AddSpotFactory<TestSpot>(
                    "test", factory => factory.DisableRelocation())
                .AddEntrySpot<TestEntrySpot>();
        });

        var registration = services.BuildServiceProvider().GetRequiredService<ZLinkFrameworkRegistration>();
        var assemblies = registration.EnumerateHandlerScanAssemblies().ToArray();

        Assert.Contains(typeof(TestHeaderSession).Assembly, assemblies);
        Assert.Contains(typeof(TestSpot).Assembly, assemblies);
        Assert.Contains(typeof(TestEntrySpot).Assembly, assemblies);
        Assert.Contains(registration.ScannedHandlerCatalog.SessionHandlers, static candidate =>
            candidate is ZLinkScannedAttributedSessionHandler attributed
            && attributed.SessionType == typeof(TestHeaderSession));
    }

    [Fact]
    public async Task FrozenSessionHandlerCatalogCanBindRepeatedRegistriesWithoutRescanning()
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.AddStreamNode("client.stream")
                .Bind("tcp://127.0.0.1:9100")
                .AddSession<TestHeaderSession>();
        });
        using var provider = services.BuildServiceProvider();
        var registration = provider.GetRequiredService<ZLinkFrameworkRegistration>();
        var candidates = registration.ScannedHandlerCatalog.SessionHandlers;

        var firstContext = new TestSessionPacketContext();
        await using var firstHandlerInstances = new ZLinkScopedHandlerInstanceOwner(provider);
        var first = new ZLinkSessionHandlerRegistry(firstHandlerInstances);
        first.BindContext(firstContext);
        first.AddScannedHandlers(candidates);
        first.Bind();

        var secondContext = new TestSessionPacketContext();
        await using var secondHandlerInstances = new ZLinkScopedHandlerInstanceOwner(provider);
        var second = new ZLinkSessionHandlerRegistry(secondHandlerInstances);
        second.BindContext(secondContext);
        second.AddScannedHandlers(candidates);
        second.Bind();

        Assert.Same(candidates, registration.ScannedHandlerCatalog.SessionHandlers);
        Assert.True(await first.TryHandleAsync(
            new ZLinkSessionDispatchContext(nameof(TestSessionPacketMessage)),
            ZLinkMessage.From(new TestSessionPacketMessage())));
        Assert.True(await second.TryHandleAsync(
            new ZLinkSessionDispatchContext(nameof(TestSessionPacketMessage)),
            ZLinkMessage.From(new TestSessionPacketMessage())));
        Assert.Equal(1, firstContext.HandledCount);
        Assert.Equal(1, secondContext.HandledCount);
    }

    [Fact]
    public void Registration_Can_Disable_Implicit_Handler_Auto_Registration()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            options.DisableImplicitHandlerAutoRegistration();
            options.AddStreamNode("client.stream")
                .Bind("tcp://127.0.0.1:9100")
                .AddSession<TestHeaderSession>();
            var node = options.AddRouteMesh("stage-node")
                .Listen("tcp://127.0.0.1:9000");
            node.Channel("stage-node").Server();
            node.Objects().Server().AddSpotFactory<TestSpot>(
                "test", factory => factory.DisableRelocation());
        });

        var registration = services.BuildServiceProvider().GetRequiredService<ZLinkFrameworkRegistration>();

        Assert.Empty(registration.EnumerateHandlerScanAssemblies());
    }

    [Fact]
    public void Registration_Disabling_Implicit_Auto_Registration_Keeps_Explicit_Handler_Assemblies()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            options.DisableImplicitHandlerAutoRegistration();
            options.AddHandlersFromAssembly(typeof(TestSessionPacketHandler).Assembly);
            var node = options.AddRouteMesh("stage-node")
                .Listen("tcp://127.0.0.1:9000");
            node.Channel("stage-node").Server();
            node.Objects().Server().AddSpotFactory<TestSpot>(
                "test", factory => factory.DisableRelocation());
        });

        var registration = services.BuildServiceProvider().GetRequiredService<ZLinkFrameworkRegistration>();
        var assembly = Assert.Single(registration.EnumerateHandlerScanAssemblies());

        Assert.Equal(typeof(TestSessionPacketHandler).Assembly, assembly);
    }

    [Fact]
    public void SessionHandlerRegistry_Throws_When_PacketName_Is_Duplicated()
    {
        using var provider = new ServiceCollection()
            .AddScoped<DuplicateSessionPacketHandler>()
            .AddScoped<SecondDuplicateSessionPacketHandler>()
            .BuildServiceProvider();
        var handlerInstances = new ZLinkScopedHandlerInstanceOwner(provider);
        var registry = new ZLinkSessionHandlerRegistry(handlerInstances);
        registry.BindContext(new DuplicateSessionPacketContext());
        registry.AddHandler<DuplicateSessionPacketHandler>();

        var exception = Assert.Throws<ZLinkConfigurationException>(() =>
            registry.AddHandler<SecondDuplicateSessionPacketHandler>());

        Assert.Contains($"Session packet handler '{nameof(DuplicateSessionPacketMessage)}' is already registered",
            exception.Message,
            StringComparison.Ordinal);
    }

    [Fact]
    public async Task AddZLinkFramework_Registers_SpotPublisher_Client()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
            {
                var mesh = options.AddRouteMesh("stage-node");
                mesh.Channel("stage-node").Server();
                {
                    var spot = mesh;
                    {
                        var router = spot.Listen("tcp://127.0.0.1:6204");
                    }
                }
            }
        });

        await using var provider = services.BuildServiceProvider();
        Assert.NotNull(provider.GetService<IZLinkSpotPublisherClient>());
    }

    [Fact]
    public void AddZLinkFramework_Registers_BoundSession_Factory()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            var mesh = options.AddRouteMesh("gateway")
                .Listen("tcp://127.0.0.1:6202")
                .SetRoutingId(RoutingId.From("gateway"));
            mesh.Channel("gateway").Server();
        });

        using var provider = services.BuildServiceProvider();
    }

    [Fact]
    public async Task AddZLinkFramework_Registers_Internal_ActorResolver_With_LocationStore_Without_ActorFactory()
    {
        var services = new ServiceCollection();

        services.AddZLinkFramework(options =>
        {
            options.UseTestLocationStore();
        });

        await using var provider = services.BuildServiceProvider();
        Assert.NotNull(provider.GetService<IZLinkActorResolver>());
        Assert.Null(provider.GetService<IZLinkActorManager>());
    }

    [Fact]
    public async Task AddZLinkFramework_AddLocationStores_ResolvesEveryStoreRoleToOneInstance()
    {
        var services = new ServiceCollection();
        var backing = new ZLinkInMemoryLocationStore();

        services.AddZLinkFramework(options =>
        {
            // Extension-package style registration: one physical store
            // instance backs every location store contract, the way codecs
            // register serializer instances.
            options.AddLocationStore(backing);
        });

        await using var provider = services.BuildServiceProvider();

        Assert.Same(backing, provider.GetRequiredService<IZLinkLocationRepository>());

        // The location runtime surface comes up on top of the hook exactly
        // as it does for the per-role registrations.
        Assert.NotNull(provider.GetService<IZLinkLocationRuntimeQuery>());
        Assert.NotNull(provider.GetService<IZLinkMeshNodeLocationResolver>());
        Assert.Same(
            provider.GetRequiredService<ZLinkLocationLifecycle>().ActorOwnership,
            provider.GetRequiredService<IZLinkActorLocationLifecycle>());
    }

    [Fact]
    public void LocationPolicy_UsesTheExactContractDefaults()
    {
        var options = new ZLinkLocationOptions();

        Assert.Equal(TimeSpan.FromSeconds(5), options.OwnerLeaseRenewInterval);
        Assert.Equal(TimeSpan.FromSeconds(15), options.OwnerLeaseTtl);
        Assert.Equal(TimeSpan.FromSeconds(5), options.OwnerLeaseFencingMargin);
        Assert.Equal(TimeSpan.FromSeconds(3), options.OwnerLeaseRenewTimeout);
        Assert.Equal(TimeSpan.FromSeconds(15), options.RouteCacheMaxAge);
        Assert.Equal(TimeSpan.FromSeconds(30), options.MessageFollowDuration);
        Assert.Equal(64, options.MaxActiveOutboundRelocations);
        Assert.Equal(64, options.MaxActiveInboundRelocations);
        Assert.Equal(8, options.MaxConcurrentRelocationCaptures);
        Assert.Equal(8, options.MaxConcurrentRelocationRestores);
        Assert.Equal(268_435_456, options.MaxRelocationPayloadInFlightBytes);
    }

    [Theory]
    [InlineData(-1, 30)]
    [InlineData(15, -1)]
    [InlineData(26, 30)]
    public void AddZLinkFramework_RejectsInvalidObjectRoutingTimes(
        int cacheSeconds,
        int messageFollowSeconds)
    {
        var exception = Assert.Throws<ZLinkConfigurationException>(() =>
            new ServiceCollection().AddZLinkFramework(options =>
            {
                var locations = options.ConfigureLocations();
                locations.RouteCacheMaxAge = TimeSpan.FromSeconds(cacheSeconds);
                locations.MessageFollowDuration =
                    TimeSpan.FromSeconds(messageFollowSeconds);
            }));

        Assert.Contains(
            cacheSeconds < 0 || messageFollowSeconds < 0
                ? "greater than or equal to zero"
                : "at least five seconds shorter",
            exception.Message,
            StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("outbound")]
    [InlineData("inbound")]
    [InlineData("capture")]
    [InlineData("restore")]
    [InlineData("payload")]
    public void AddZLinkFramework_RejectsNonPositiveRelocationLimits(string limit)
    {
        var exception = Assert.Throws<ZLinkConfigurationException>(() =>
            new ServiceCollection().AddZLinkFramework(options =>
            {
                var locations = options.ConfigureLocations();
                switch (limit)
                {
                    case "outbound":
                        locations.MaxActiveOutboundRelocations = 0;
                        break;
                    case "inbound":
                        locations.MaxActiveInboundRelocations = 0;
                        break;
                    case "capture":
                        locations.MaxConcurrentRelocationCaptures = 0;
                        break;
                    case "restore":
                        locations.MaxConcurrentRelocationRestores = 0;
                        break;
                    case "payload":
                        locations.MaxRelocationPayloadInFlightBytes = 0;
                        break;
                }
            }));

        Assert.Contains("greater than zero", exception.Message, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("polling")]
    [InlineData("store-grace")]
    public void AddZLinkFramework_RejectsNonPositiveLocationPollingTimes(string option)
    {
        var exception = Assert.Throws<ZLinkConfigurationException>(() =>
            new ServiceCollection().AddZLinkFramework(framework =>
            {
                var locations = framework.ConfigureLocations();
                if (option == "polling")
                    locations.PollingInterval = TimeSpan.Zero;
                else
                    locations.StoreFailureGrace = TimeSpan.Zero;
            }));

        Assert.Contains("greater than zero", exception.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AddZLinkFramework_Throws_WhenAddLocationStoreIsCombinedWithInMemoryStore()
    {
        var inMemory = new ServiceCollection();
        var inMemoryConflict = Assert.Throws<ZLinkConfigurationException>(() =>
            inMemory.AddZLinkFramework(options =>
            {
                options.UseTestLocationStore();
                options.AddLocationStore(new ZLinkInMemoryLocationStore());
            }));
        Assert.Contains("AddLocationStore", inMemoryConflict.Message, StringComparison.Ordinal);

    }

    [Fact]
    public async Task AddLocationStore_Instance_Is_Disposed_Exactly_Once_By_The_Host_Provider()
    {
        var store = DispatchProxy.Create<ITrackedLocationStore, TrackedLocationStoreProxy>();
        var tracker = (TrackedLocationStoreProxy)(object)store;
        var services = new ServiceCollection();
        services.AddZLinkFramework(options => options.AddLocationStore(store));
        var provider = services.BuildServiceProvider();

        Assert.Same(
            store,
            provider.GetRequiredService<
                Zlink.Framework.LocationProvider.IZLinkLocationStore>());
        Assert.Same(
            store,
            provider.GetRequiredService<
                Zlink.Framework.LocationProvider.IZLinkLocationStore>());
        _ = provider.GetServices<IHostedService>().ToArray();
        await provider.DisposeAsync();

        Assert.Equal(1, tracker.DisposeCount);
    }

    [Fact]
    public async Task HostStartup_Accepts_PublicLocationStore_WithoutHiddenProjection()
    {
        var store = DispatchProxy.Create<ITrackedLocationStore, TrackedLocationStoreProxy>();
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(options =>
        {
            options.ConfigureInboundDispatch().ApplicationHwmBytes = 0;
            options.AddLocationStore(store);
        });
        using var host = builder.Build();

        await host.StartAsync();
        await host.StopAsync();
    }

    [Fact]
    public async Task HostStartup_Rejects_Duplicate_UserSpot_Packet_Registration()
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(options =>
        {
            options.ConfigureInboundDispatch().ApplicationHwmBytes = 0;
            options.UseTestLocationStore();
            options.DisableImplicitHandlerAutoRegistration();
            var node = options.AddRouteMesh("duplicate-packet")
                .Listen($"inproc://duplicate-packet-{Guid.NewGuid():N}");
            node.Channel("duplicate-packet").Server();
            node.Objects().Server().AddSpotFactory<DuplicatePacketSpot>(
                "duplicate-packet", factory => factory.DisableRelocation());
        });
        using var host = builder.Build();

        var exception = await Assert.ThrowsAsync<ZLinkConfigurationException>(() => host.StartAsync());

        Assert.Contains(
            $"SPOT packet handler '{nameof(DuplicatePacketMessage)}' is already registered",
            exception.Message,
            StringComparison.Ordinal);
    }

    [Fact]
    public async Task HostStartup_Rejects_Duplicate_UserSpot_Subscription_Registration()
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(options =>
        {
            options.ConfigureInboundDispatch().ApplicationHwmBytes = 0;
            options.UseTestLocationStore();
            options.DisableImplicitHandlerAutoRegistration();
            var node = options.AddRouteMesh("duplicate-subscription")
                .Listen($"inproc://duplicate-subscription-{Guid.NewGuid():N}");
            node.Channel("duplicate-subscription").Server();
            node.Objects().Server().AddSpotFactory<DuplicateSubscriptionSpot>(
                "duplicate-subscription", factory => factory.DisableRelocation());
        });
        using var host = builder.Build();

        var exception = await Assert.ThrowsAsync<ZLinkConfigurationException>(() => host.StartAsync());

        Assert.Contains("SPOT subscription handler", exception.Message, StringComparison.Ordinal);
        Assert.Contains("already registered", exception.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task HostStartup_Rejects_Duplicate_UserSpot_Actor_Registration()
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(options =>
        {
            options.ConfigureInboundDispatch().ApplicationHwmBytes = 0;
            options.UseTestLocationStore();
            options.DisableImplicitHandlerAutoRegistration();
            var node = options.AddRouteMesh("duplicate-actor")
                .Listen($"inproc://duplicate-actor-{Guid.NewGuid():N}");
            node.Channel("duplicate-actor").Server();
            node.Objects().Server().AddSpotFactory<DuplicateActorSpot>(
                "duplicate-actor", factory => factory.DisableRelocation());
        });
        using var host = builder.Build();

        var exception = await Assert.ThrowsAsync<ZLinkConfigurationException>(() => host.StartAsync());

        Assert.Contains("actor packet", exception.Message, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("already registered", exception.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task HostStartup_Rejects_Invalid_ScannedSpotTimer()
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(options =>
        {
            options.ConfigureInboundDispatch().ApplicationHwmBytes = 0;
            options.UseTestLocationStore();
            var node = options.AddRouteMesh("invalid-timer")
                .Listen($"inproc://invalid-timer-{Guid.NewGuid():N}");
            node.Channel("invalid-timer").Server();
            node.Objects().Server().AddSpotFactory<InvalidTimerSpot>(
                "invalid-timer", factory => factory.DisableRelocation());
        });
        using var host = builder.Build();

        var exception = await Assert.ThrowsAsync<ZLinkConfigurationException>(() => host.StartAsync());

        Assert.Contains("SPOT timer period must be greater than zero", exception.Message, StringComparison.Ordinal);
    }

    private sealed record DuplicatePacketMessage(string Value);

    private sealed class DuplicatePacketSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;

        public void Configure()
        {
            Context.Handlers.AddPacket<FirstDuplicatePacketHandler>();
            Context.Handlers.AddPacket<SecondDuplicatePacketHandler>();
        }
    }

    private sealed class FirstDuplicatePacketHandler
        : IZLinkSpotPacketHandler<DuplicatePacketSpot, DuplicatePacketMessage>
    {
        public ValueTask HandleAsync(
            DuplicatePacketSpot spot,
            DuplicatePacketMessage message,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }

    private sealed class SecondDuplicatePacketHandler
        : IZLinkSpotPacketHandler<DuplicatePacketSpot, DuplicatePacketMessage>
    {
        public ValueTask HandleAsync(
            DuplicatePacketSpot spot,
            DuplicatePacketMessage message,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }

    private sealed record DuplicateSubscriptionMessage(string Value);

    private sealed class DuplicateSubscriptionSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;

        public void Configure()
        {
            Context.Handlers.AddSubscribe<DuplicateSubscriptionHandler>("duplicate-channel", "duplicate-topic");
            Context.Handlers.AddSubscribe<DuplicateSubscriptionHandler>("duplicate-channel", "duplicate-topic");
        }
    }

    private sealed class DuplicateSubscriptionHandler
        : IZLinkSpotSubscriptionHandler<DuplicateSubscriptionSpot, DuplicateSubscriptionMessage>
    {
        public ValueTask HandleAsync(
            DuplicateSubscriptionSpot spot,
            DuplicateSubscriptionMessage message,
            ZLinkPublishMessageContext context,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }

    private sealed record DuplicateActorMessage(string Value);

    private sealed class DuplicateActor(string actorId, IZLinkActorContext context) : IZLinkActor
    {
        public string ActorId { get; } = actorId;

        public IZLinkActorContext Context { get; } = context;
    }

    private sealed class DuplicateActorSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;

        public void Configure()
        {
            Context.Handlers.AddActorPacket<DuplicateActorHandler, DuplicateActor>();
            Context.Handlers.AddActorPacket<SecondDuplicateActorHandler, DuplicateActor>();
        }
    }

    private sealed class DuplicateActorHandler
        : IZLinkSpotActorSendHandler<DuplicateActorSpot, DuplicateActor, DuplicateActorMessage>
    {
        public ValueTask HandleAsync(
            DuplicateActorSpot spot,
            DuplicateActor actor,
            IZLinkMessageContext context,
            DuplicateActorMessage message,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }

    private sealed class SecondDuplicateActorHandler
        : IZLinkSpotActorSendHandler<DuplicateActorSpot, DuplicateActor, DuplicateActorMessage>
    {
        public ValueTask HandleAsync(
            DuplicateActorSpot spot,
            DuplicateActor actor,
            IZLinkMessageContext context,
            DuplicateActorMessage message,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }

    private sealed class InvalidTimerSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
    }

    [ZLinkSpotTimerHandler("invalid", 0)]
    private sealed class InvalidTimerHandler : IZLinkSpotTimerHandler<InvalidTimerSpot>
    {
        public ValueTask HandleAsync(
            InvalidTimerSpot spot,
            ZLinkTimerTick tick,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }

    private interface ITrackedLocationStore :
        Zlink.Framework.LocationProvider.IZLinkLocationStore,
        IAsyncDisposable;

    private class TrackedLocationStoreProxy : DispatchProxy
    {
        private readonly ZLinkInMemoryProviderLocationStore _inner = new();

        public int DisposeCount { get; private set; }

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            Assert.NotNull(targetMethod);
            if (targetMethod.DeclaringType == typeof(IAsyncDisposable))
            {
                DisposeCount++;
                return ValueTask.CompletedTask;
            }

            return targetMethod.Invoke(_inner, args);
        }
    }

}
