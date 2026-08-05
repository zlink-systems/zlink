using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Configuration;

/// <summary>
///     Worked examples for the two role-split builder families of
///     03-configuration-topology: a ClientServer channel registration picks
///     <c>Client()</c> or <c>Server()</c> before it can be configured, and a
///     MeshNode's object registrations do the same through
///     <c>Objects()</c>. Both client builders are deliberately empty because
///     a client registers nothing beyond its role.
/// </summary>
public sealed class ObjectAndChannelRoleBuilderContracts
{
    [Fact]
    [ContractExample(
        typeof(IZLinkClientServerChannelRoleBuilder),
        typeof(IZLinkClientServerChannelClientBuilder),
        typeof(IZLinkClientServerChannelServerBuilder))]
    public void Client_server_channel_registers_a_connect_side_or_a_listening_handler_side()
    {
        // Two processes register the same channel name from opposite sides.
        // The gateway consumes the inventory API, so it only connects.
        var gatewayChannel = new ExampleClientServerChannelRoleBuilder("inventory");
        var client = gatewayChannel.Client();
        Assert.Same(client, client.Connect("tcp://inventory-1.play.svc:5100"));
        Assert.Same(client, client.Connect("tcp://inventory-2.play.svc:5100"));
        Assert.Equal(
            ["tcp://inventory-1.play.svc:5100", "tcp://inventory-2.play.svc:5100"],
            gatewayChannel.ClientBuilder.Endpoints);

        // The inventory service registers the serving side: a bound endpoint,
        // its placement weight and the handlers that answer the channel.
        var inventoryChannel = new ExampleClientServerChannelRoleBuilder("inventory");
        var server = inventoryChannel.Server();
        Assert.Same(server, server.Listen(5100));
        Assert.Same(server, server.SetBindHost("0.0.0.0"));
        Assert.Same(server, server.SetAdvertiseHost("inventory-1.play.svc"));
        Assert.Same(server, server.SetWeight(100));
        Assert.Same(server, server.AddHandlerGroup("inventory-write"));
        Assert.Same(
            server,
            server.AddSendHandler<ItemGrantedHandler, ItemGranted>("inventory.item-granted"));
        Assert.Same(
            server,
            server.AddRequestHandler<ConsumeItemHandler, ConsumeItem, ItemConsumed>());

        var registered = inventoryChannel.ServerBuilder;
        Assert.Equal(5100, registered.Port);
        Assert.Equal("0.0.0.0", registered.BindHost);
        Assert.Equal("inventory-1.play.svc", registered.AdvertiseHost);
        Assert.Equal(100, registered.Weight);
        Assert.Equal(["inventory-write"], registered.HandlerGroups);
        Assert.Equal(
            [
                "inventory.item-granted -> ItemGrantedHandler",
                "(default) -> ConsumeItemHandler"
            ],
            registered.Handlers);

        // The role split is what keeps a connect-only registration from
        // carrying listen or handler configuration at all.
        Assert.Equal(
            new[] { "Connect" },
            typeof(IZLinkClientServerChannelClientBuilder)
                .GetMethods()
                .Select(method => method.Name)
                .ToArray());
        Assert.Null(typeof(IZLinkClientServerChannelRoleBuilder).GetMethod("Listen"));
        Assert.Null(typeof(IZLinkClientServerChannelServerBuilder).GetMethod("Connect"));
    }

    [Fact]
    [ContractExample(
        typeof(IZLinkMeshObjectRoleBuilder),
        typeof(IZLinkMeshObjectClientBuilder),
        typeof(IZLinkMeshObjectServerBuilder))]
    public void Mesh_object_role_builder_puts_every_factory_registration_on_the_server_side()
    {
        // A gateway node joins the play mesh to address Spots and Actors but
        // hosts none of them: Client() is the whole registration, which is
        // why the client builder declares no members.
        var gatewayObjects = new ExampleMeshObjectRoleBuilder();
        var gatewayClient = gatewayObjects.Client();
        Assert.NotNull(gatewayClient);
        Assert.Equal(ZLinkMeshNodeObjectRole.Client, gatewayObjects.Role);
        Assert.Empty(typeof(IZLinkMeshObjectClientBuilder).GetMembers());
        Assert.DoesNotContain(
            typeof(IZLinkMeshObjectServerBuilder),
            typeof(IZLinkMeshNodeBuilder).GetInterfaces());

        // A play node hosts objects, so every factory registration - and the
        // relocation policy that governs how each moves - lands on Server().
        var playObjects = new ExampleMeshObjectRoleBuilder();
        var server = playObjects.Server();
        Assert.Equal(ZLinkMeshNodeObjectRole.Server, playObjects.Role);

        Assert.Same(server, server.AddEntrySpot<LobbyEntrySpot>());
        Assert.Same(
            server,
            server.AddSpotFactory<BattleRoomSpot>(
                "battle-room",
                factory => factory
                    .StableTypeLimit(2000)
                    .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                    .RecreateOnRelocation()));
        Assert.Same(
            server,
            server.AddInstanceSpotFactory<LeaderboardSpot>(
                "leaderboard",
                factory => factory
                    .StableTypeLimit(64)
                    .DisableRelocation()));
        Assert.Same(
            server,
            server.AddActorFactory<PlayerActor, PlayerActorFactory>(
                "player", factory => factory.PreserveStateWith<PlayerRelocationAdapter>()));

        var registered = playObjects.ServerBuilder;
        Assert.Equal(["LobbyEntrySpot"], registered.EntrySpots);
        Assert.Equal(["battle-room"], registered.SpotTypes);
        Assert.Equal(["leaderboard"], registered.InstanceSpotTypes);
        Assert.Equal(["player"], registered.ActorTypes);
        Assert.Equal(64, registered.InstanceSpotStableTypeLimit);
        Assert.Equal(
            ZLinkUserSpotExecutionMode.SpotWide,
            registered.SpotExecutionMode);
        Assert.Equal("Recreate", registered.SpotRelocation);
        Assert.Equal("Disabled", registered.InstanceSpotRelocation);
        Assert.Equal(
            nameof(PlayerRelocationAdapter),
            registered.ActorRelocation);
    }

    private sealed record ItemGranted(string ActorId, string ItemId);

    private sealed record ConsumeItem(string ActorId, string ItemId);

    private sealed record ItemConsumed(string ActorId, int Remaining);

    private sealed class ItemGrantedHandler : IZLinkSendHandler<ItemGranted>
    {
        public ValueTask HandleAsync(
            ItemGranted message,
            IZLinkMessageContext context,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }

    private sealed class ConsumeItemHandler : IZLinkRequestHandler<ConsumeItem, ItemConsumed>
    {
        public ValueTask<ItemConsumed> HandleAsync(
            ConsumeItem request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken) =>
            ValueTask.FromResult(new ItemConsumed(request.ActorId, 2));
    }

    private sealed class ExampleClientServerChannelRoleBuilder(string channelName)
        : IZLinkClientServerChannelRoleBuilder
    {
        public string ChannelName { get; } = channelName;

        public ExampleClientServerChannelClientBuilder ClientBuilder { get; } = new();

        public ExampleClientServerChannelServerBuilder ServerBuilder { get; } = new();

        IZLinkClientServerChannelClientBuilder IZLinkClientServerChannelRoleBuilder.Client() =>
            ClientBuilder;

        IZLinkClientServerChannelServerBuilder IZLinkClientServerChannelRoleBuilder.Server() =>
            ServerBuilder;

        public IZLinkClientServerChannelClientBuilder Client() => ClientBuilder;

        public IZLinkClientServerChannelServerBuilder Server() => ServerBuilder;
    }

    private sealed class ExampleClientServerChannelClientBuilder
        : IZLinkClientServerChannelClientBuilder
    {
        public List<string> Endpoints { get; } = [];

        public IZLinkClientServerChannelClientBuilder Connect(string endpoint)
        {
            Endpoints.Add(endpoint);
            return this;
        }
    }

    private sealed class ExampleClientServerChannelServerBuilder
        : IZLinkClientServerChannelServerBuilder
    {
        public int Port { get; private set; }

        public string? BindHost { get; private set; }

        public string? AdvertiseHost { get; private set; }

        public int Weight { get; private set; }

        public List<string> HandlerGroups { get; } = [];

        public List<string> Handlers { get; } = [];

        public IZLinkClientServerChannelServerBuilder Listen(int port = 0)
        {
            Port = port;
            return this;
        }

        public IZLinkClientServerChannelServerBuilder SetBindHost(string bindHost)
        {
            BindHost = bindHost;
            return this;
        }

        public IZLinkClientServerChannelServerBuilder SetAdvertiseHost(string advertiseHost)
        {
            AdvertiseHost = advertiseHost;
            return this;
        }

        public IZLinkClientServerChannelServerBuilder SetWeight(int weight)
        {
            Weight = weight;
            return this;
        }

        public IZLinkClientServerChannelServerBuilder AddHandlerGroup(string groupName)
        {
            HandlerGroups.Add(groupName);
            return this;
        }

        public IZLinkClientServerChannelServerBuilder AddSendHandler<THandler, TMessage>(
            string? packetName = null)
            where THandler : class, IZLinkSendHandler<TMessage>
        {
            Handlers.Add($"{packetName ?? "(default)"} -> {typeof(THandler).Name}");
            return this;
        }

        public IZLinkClientServerChannelServerBuilder AddRequestHandler<THandler, TRequest, TReply>(
            string? packetName = null)
            where THandler : class, IZLinkRequestHandler<TRequest, TReply>
        {
            Handlers.Add($"{packetName ?? "(default)"} -> {typeof(THandler).Name}");
            return this;
        }
    }

    private sealed class ExampleMeshObjectRoleBuilder : IZLinkMeshObjectRoleBuilder
    {
        public ZLinkMeshNodeObjectRole Role { get; private set; } = ZLinkMeshNodeObjectRole.None;

        public ExampleMeshObjectClientBuilder ClientBuilder { get; } = new();

        public ExampleMeshObjectServerBuilder ServerBuilder { get; } = new();

        public IZLinkMeshObjectClientBuilder Client()
        {
            Role = ZLinkMeshNodeObjectRole.Client;
            return ClientBuilder;
        }

        public IZLinkMeshObjectServerBuilder Server()
        {
            Role = ZLinkMeshNodeObjectRole.Server;
            return ServerBuilder;
        }
    }

    private sealed class ExampleMeshObjectClientBuilder : IZLinkMeshObjectClientBuilder;

    private sealed class ExampleMeshObjectServerBuilder : IZLinkMeshObjectServerBuilder
    {
        public List<string> EntrySpots { get; } = [];

        public List<string> SpotTypes { get; } = [];

        public List<string> InstanceSpotTypes { get; } = [];

        public List<string> ActorTypes { get; } = [];

        public int SpotStableTypeLimit { get; private set; }

        public ZLinkUserSpotExecutionMode SpotExecutionMode { get; private set; }

        public string? SpotRelocation { get; private set; }

        public int InstanceSpotStableTypeLimit { get; private set; }

        public string? InstanceSpotRelocation { get; private set; }

        public string? ActorRelocation { get; private set; }

        public IZLinkMeshObjectServerBuilder AddEntrySpot<TEntrySpot>()
            where TEntrySpot : class, IZLinkEntrySpot
        {
            EntrySpots.Add(typeof(TEntrySpot).Name);
            return this;
        }

        public IZLinkMeshObjectServerBuilder AddSpotFactory<TSpot>(
            string spotType,
            Action<IZLinkUserSpotFactoryBuilder<TSpot>> configure)
            where TSpot : class, IZLinkSpot
        {
            SpotTypes.Add(spotType);
            var factory = new RecordingUserSpotFactoryBuilder<TSpot>();
            configure(factory);
            SpotStableTypeLimit = factory.StableTypeLimitValue;
            SpotExecutionMode = factory.ExecutionModeValue;
            SpotRelocation = factory.Relocation;
            return this;
        }

        public IZLinkMeshObjectServerBuilder AddInstanceSpotFactory<TSpot>(
            string instanceSpotType,
            Action<IZLinkInstanceSpotFactoryBuilder<TSpot>> configure)
            where TSpot : class, IZLinkInstanceSpot
        {
            InstanceSpotTypes.Add(instanceSpotType);
            var factory = new RecordingInstanceSpotFactoryBuilder<TSpot>();
            configure(factory);
            InstanceSpotStableTypeLimit = factory.StableTypeLimitValue;
            InstanceSpotRelocation = factory.Relocation;
            return this;
        }

        public IZLinkMeshObjectServerBuilder AddActorFactory<TActor, TFactory>(
            string actorType,
            Action<IZLinkActorFactoryBuilder<TActor>> configure)
            where TActor : class, IZLinkActor
            where TFactory : class, IZLinkActorFactory<TActor>
        {
            ActorTypes.Add(actorType);
            var factory = new RecordingActorFactoryBuilder<TActor>();
            configure(factory);
            ActorRelocation = factory.Relocation;
            return this;
        }
    }

    private sealed class RecordingActorFactoryBuilder<TActor>
        : IZLinkActorFactoryBuilder<TActor>
        where TActor : class, IZLinkActor
    {
        public string? Relocation { get; private set; }

        public IZLinkActorFactoryBuilder<TActor> DisableRelocation() =>
            Select("Disabled");

        public IZLinkActorFactoryBuilder<TActor> RecreateOnRelocation() =>
            Select("Recreate");

        public IZLinkActorFactoryBuilder<TActor> PreserveStateWith<TAdapter>()
            where TAdapter : class, IZLinkActorRelocationAdapter<TActor> =>
            Select(typeof(TAdapter).Name);

        private IZLinkActorFactoryBuilder<TActor> Select(string relocation)
        {
            Relocation = relocation;
            return this;
        }
    }

    private sealed class RecordingUserSpotFactoryBuilder<TSpot>
        : IZLinkUserSpotFactoryBuilder<TSpot>
        where TSpot : class, IZLinkSpot
    {
        public int StableTypeLimitValue { get; private set; }

        public ZLinkUserSpotExecutionMode ExecutionModeValue { get; private set; }

        public string? Relocation { get; private set; }

        public IZLinkUserSpotFactoryBuilder<TSpot> StableTypeLimit(int limit)
        {
            StableTypeLimitValue = limit;
            return this;
        }

        public IZLinkUserSpotFactoryBuilder<TSpot> ExecutionMode(
            ZLinkUserSpotExecutionMode mode)
        {
            ExecutionModeValue = mode;
            return this;
        }

        public IZLinkUserSpotFactoryBuilder<TSpot> RelocationReadiness(
            ZLinkSpotRelocationReadinessMode mode) => this;

        public IZLinkUserSpotFactoryBuilder<TSpot> DisableRelocation() =>
            Select("Disabled");

        public IZLinkUserSpotFactoryBuilder<TSpot> RecreateOnRelocation() =>
            Select("Recreate");

        public IZLinkUserSpotFactoryBuilder<TSpot> PreserveStateWith<TAdapter>()
            where TAdapter : class, IZLinkSpotRelocationAdapter<TSpot> =>
            Select(typeof(TAdapter).Name);

        private IZLinkUserSpotFactoryBuilder<TSpot> Select(string relocation)
        {
            Relocation = relocation;
            return this;
        }
    }

    private sealed class RecordingInstanceSpotFactoryBuilder<TSpot>
        : IZLinkInstanceSpotFactoryBuilder<TSpot>
        where TSpot : class, IZLinkInstanceSpot
    {
        public int StableTypeLimitValue { get; private set; }

        public string? Relocation { get; private set; }

        public IZLinkInstanceSpotFactoryBuilder<TSpot> StableTypeLimit(int limit)
        {
            StableTypeLimitValue = limit;
            return this;
        }

        public IZLinkInstanceSpotFactoryBuilder<TSpot> DisableRelocation() =>
            Select("Disabled");

        public IZLinkInstanceSpotFactoryBuilder<TSpot> RecreateOnRelocation() =>
            Select("Recreate");

        public IZLinkInstanceSpotFactoryBuilder<TSpot> PreserveStateWith<TAdapter>()
            where TAdapter : class, IZLinkSpotRelocationAdapter<TSpot> =>
            Select(typeof(TAdapter).Name);

        private IZLinkInstanceSpotFactoryBuilder<TSpot> Select(string relocation)
        {
            Relocation = relocation;
            return this;
        }
    }

    private sealed class LobbyEntrySpot : IZLinkEntrySpot
    {
        public IZLinkEntrySpotContext Context => null!;
    }

    private sealed class BattleRoomSpot : IZLinkSpot
    {
        public IZLinkSpotContext Context => null!;
    }

    private sealed class LeaderboardSpot : IZLinkInstanceSpot
    {
        public IZLinkInstanceSpotContext Context => null!;
    }

    private sealed class PlayerActor : IZLinkActor
    {
        public IZLinkActorContext Context => null!;
    }

    private sealed class PlayerActorFactory : IZLinkActorFactory<PlayerActor>
    {
        public ValueTask<PlayerActor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(new PlayerActor());
    }

    private sealed class PlayerRelocationAdapter : IZLinkActorRelocationAdapter<PlayerActor>
    {
        public ValueTask<byte[]> CaptureAsync(
            PlayerActor actor,
            CancellationToken cancellationToken) => ValueTask.FromResult(Array.Empty<byte>());

        public ValueTask RestoreAsync(
            PlayerActor actor,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }
}
