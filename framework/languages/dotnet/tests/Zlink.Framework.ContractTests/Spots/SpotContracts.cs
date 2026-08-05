using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Workers;
using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Spots;

public sealed class SpotContracts
{
    [Fact]
    public void Actor_join_and_relocation_public_contract_matches_common_spec()
    {
        var lifecycle = typeof(IZLinkUserSpotActorLifecycle<>).MakeGenericType(typeof(PlayerActor));
        var join = lifecycle.GetMethod(nameof(IZLinkSpot<PlayerActor>.OnActorJoinAsync));
        Assert.NotNull(join);
        Assert.True(join.IsAbstract);
        Assert.Collection(
            join.GetParameters(),
            actorId => Assert.Equal(typeof(string), actorId.ParameterType),
            request => Assert.Equal(typeof(ZLinkMessage), request.ParameterType),
            cancellation => Assert.Equal(typeof(CancellationToken), cancellation.ParameterType));

        // Join admission lives on the User Spot lifecycle; membership callbacks live on the
        // shared membership lifecycle that both User and Entry Spots implement.
        var membership = typeof(IZLinkSpotActorMembershipLifecycle<>).MakeGenericType(typeof(PlayerActor));
        Assert.True(membership.GetMethod(nameof(IZLinkSpot<PlayerActor>.OnJoinedActorAsync))!.IsAbstract);
        Assert.True(membership.GetMethod(nameof(IZLinkSpot<PlayerActor>.OnLeaveActorAsync))!.IsAbstract);
        Assert.True(lifecycle.IsAssignableTo(membership));
        Assert.Null(typeof(IZLinkSpot).Assembly.GetType("Zlink.Framework.Contracts.Spots.ZLinkActorJoinAdmission"));
        Assert.Null(typeof(IZLinkSpot).Assembly.GetType(
            "Zlink.Framework.Contracts.Spots.IZLinkActorTransferAdapter`1"));
        Assert.NotNull(typeof(IZLinkActorRelocationAdapter<>));
        Assert.Null(typeof(IZLinkMeshNodeBuilder).GetMethod("AddStatelessActorTransfer"));
        Assert.Null(typeof(IZLinkMeshNodeBuilder).GetMethod("AddActorTransferAdapter"));
    }

    [Fact]
    [ContractExample(
        typeof(IZLinkSpot),
        typeof(IZLinkSpot<>),
        typeof(IZLinkSpotActorMembershipLifecycle<>),
        typeof(IZLinkUserSpotActorLifecycle<>),
        typeof(IZLinkEntrySpot),
        typeof(IZLinkEntrySpot<>),
        typeof(IZLinkActorHandlerRegistry),
        typeof(IZLinkSpotHandlerRegistry),
        typeof(IZLinkInstanceSpotHandlerRegistry),
        typeof(IZLinkSpotOutbound),
        typeof(IZLinkWorkerCall<>),
        typeof(IZLinkWorkerOptions),
        typeof(IZLinkSpotCommonContext),
        typeof(IZLinkSpotContext),
        typeof(IZLinkEntrySpotContext),
        typeof(IZLinkTimer))]
    public async Task Spot_context_registers_handlers_timers_actor_lifecycle_and_outbound_messages()
    {
        var context = new SpotContext("room-1");
        IZLinkSpot spot = new RoomSpot(context);
        var entryContext = new EntrySpotContext("entry");
        IZLinkEntrySpot<PlayerActor> entrySpot = new EntrySpot(entryContext);
        var actor = new PlayerActor("player-1");

        spot.Configure();
        entrySpot.Configure();
        context.Handlers.AddHandler<RoomPacketHandler>();
        context.Handlers.AddHandler<RoomPacketHandler>("room.packet");
        context.Handlers.AddPacket<RoomPacketHandler>();
        context.Handlers.AddSubscribe<RoomEventHandler>("play-events", "room.events");
        context.Handlers.AddActorPacket<PlayerActorPacketHandler, PlayerActor>();
        context.Handlers.AddHandler<PlayerActorPacketHandler>("actor.packet");
        context.Handlers.AddHandler<PlayerActorRequestHandler>("actor.request");
        await context.LeaveActorAsync(actor);
        await entryContext.DestroyActorAsync(actor);
        await context.AddTimer<RoomTimerHandler>("heartbeat", TimeSpan.FromSeconds(1));
        await context.Outbound.SendToSpot("room-2", new RoomEvent("opened")).Async();
        await context.Outbound.RequestToSpot("room-2", new JoinRoom("room-2")).Async<JoinedRoom>();
        await context.Outbound.RequestToSpot("room-2", new JoinRoom("room-2")).Yield<JoinedRoom>();
        await context.Outbound.Publish("play-events", "room.events", new RoomEvent("opened")).Async();
        await context.Outbound.SendToChannel("api", new RoomEvent("opened")).Async();
        await context.Outbound.RequestToChannel("api", new JoinRoom("room-1")).Async<JoinedRoom>();
        await context.Outbound.RequestToChannel("api", new JoinRoom("room-1")).Yield<JoinedRoom>();

        await spot.OnCreateAsync(ZLinkMessage.Empty, CancellationToken.None);
        await spot.OnInitializeAsync(CancellationToken.None);
        var deadline = DateTimeOffset.UtcNow.AddSeconds(1);
        await spot.OnClosingAsync(
            new ZLinkSpotClosingContext(
                ZLinkSpotCloseReason.ExplicitClose,
                deadline),
            CancellationToken.None);
        await entrySpot.OnInitializeAsync(CancellationToken.None);
        await entrySpot.OnClosingAsync(
            new ZLinkSpotClosingContext(
                ZLinkSpotCloseReason.HostShutdown,
                deadline),
            CancellationToken.None);

        Assert.Equal(["player-1"], context.LeftActors);
        Assert.Equal(["player-1"], entryContext.DestroyedActors);
        Assert.Contains("heartbeat", context.Timers);
        Assert.Equal("entry", entryContext.SpotId);
    }

    [Fact]
    [ContractExample(
        typeof(IZLinkSpotCommonContext),
        typeof(IZLinkSpotContext),
        typeof(IZLinkEntrySpotContext))]
    public void Actor_destroy_is_entry_spot_context_only()
    {
        Assert.Null(typeof(IZLinkSpotContext).GetMethod("DestroyActorAsync"));
        Assert.Null(typeof(IZLinkEntrySpotContext).GetMethod("destroyActor"));

        var destroy = typeof(IZLinkEntrySpotContext).GetMethod("DestroyActorAsync");
        Assert.NotNull(destroy);
        Assert.Equal(typeof(ValueTask), destroy.ReturnType);
        Assert.Collection(
            destroy.GetParameters(),
            actor => Assert.Equal(typeof(IZLinkActor), actor.ParameterType),
            cancellation => Assert.Equal(typeof(CancellationToken), cancellation.ParameterType));
    }

    [Fact]
    public void Spot_context_identity_and_handler_surfaces_match_each_spot_kind()
    {
        Assert.NotNull(typeof(IZLinkSpotCommonContext).GetProperty("ObjectGeneration"));
        Assert.Null(typeof(IZLinkSpotCommonContext).GetProperty("Handlers"));
        Assert.Equal(
            typeof(IZLinkSpotHandlerRegistry),
            typeof(IZLinkSpotContext).GetProperty("Handlers")!.PropertyType);
        Assert.Equal(
            typeof(IZLinkSpotHandlerRegistry),
            typeof(IZLinkEntrySpotContext).GetProperty("Handlers")!.PropertyType);
        Assert.Equal(
            typeof(IZLinkInstanceSpotHandlerRegistry),
            typeof(IZLinkInstanceSpotContext).GetProperty("Handlers")!.PropertyType);
        Assert.Null(typeof(IZLinkInstanceSpotHandlerRegistry).GetMethod("AddActorPacket"));
        Assert.Null(typeof(IZLinkInstanceSpotHandlerRegistry).GetMethod("AddSubscribe"));
    }

    [Fact]
    [ContractExample(typeof(IZLinkSpotOutbound), typeof(IZLinkPublishCall))]
    public async Task Spot_outbound_context_exposes_all_routed_channel_and_publish_methods()
    {
        IZLinkSpotOutbound spotOutbound = new SpotContext("room-1");
        IZLinkSpotOutbound entryOutbound = new EntrySpotContext("entry");

        await spotOutbound.SendToSpot("room-2", new RoomEvent("spot-send")).Async();
        await spotOutbound.RequestToSpot("room-2", new JoinRoom("room-2")).Async<JoinedRoom>();
        await spotOutbound.Publish("play-events", "room.events", new RoomEvent("spot-publish")).Async();
        await spotOutbound.SendToChannel("api", new RoomEvent("spot-channel-send")).Async();
        await spotOutbound.RequestToChannel("api", new JoinRoom("room-1")).Async<JoinedRoom>();

        await entryOutbound.SendToSpot("room-2", new RoomEvent("entry-send")).Async();
        await entryOutbound.RequestToSpot("room-2", new JoinRoom("room-2")).Async<JoinedRoom>();
        await entryOutbound.Publish("play-events", "room.events", new RoomEvent("entry-publish")).Async();
        await entryOutbound.SendToChannel("api", new RoomEvent("entry-channel-send")).Async();
        await entryOutbound.RequestToChannel("api", new JoinRoom("entry")).Async<JoinedRoom>();
    }

    [Fact]
    [ContractExample(
        typeof(IZLinkSpotManager),
        typeof(IZLinkSpotClient),
        typeof(IZLinkSpotSendCall),
        typeof(IZLinkSpotRequestCall),
        typeof(IZLinkSpotOutbound),
        typeof(IZLinkSpotPublisherClient))]
    public async Task Spot_clients_separate_local_spot_api_routed_egress_and_publisher_channels()
    {
        var manager = new SpotManager();
        var created = await manager
            .GetOrCreate("room-1", "room")
            .Request(ZLinkMessage.Empty)
            .Async();
        IZLinkSpotClient localClient = new SpotOutbound();
        await localClient.SendToSpot("room-1", new RoomEvent("opened")).Async();
        var reply = await localClient.RequestToSpot("room-1", new JoinRoom("room-1")).Async<JoinedRoom>();

        IZLinkSpotPublisherClient publisher = new SpotPublisherClient();
        await publisher.Publish("play-events", "room.events", new RoomEvent("opened")).Async();

        Assert.Equal(ZLinkSpotCreateState.Created, created.State);
        Assert.Equal("room-1", reply.RoomId);
    }

    [Fact]
    [ContractExample(
        typeof(IZLinkSpotPacketHandler<,>),
        typeof(IZLinkSpotRequestHandler<,,>),
        typeof(IZLinkSpotSubscriptionHandler<,>),
        typeof(IZLinkSpotTimerHandler<>),
        typeof(IZLinkSpotActorSendHandler<,,>),
        typeof(IZLinkSpotActorRequestHandler<,,,>),
        typeof(IZLinkEntrySpotActorSendHandler<,,>),
        typeof(IZLinkEntrySpotActorRequestHandler<,,,>))]
    public async Task Spot_handlers_receive_the_spot_instance_and_actor_when_the_contract_requires_it()
    {
        var room = new RoomSpot(new SpotContext("room-1"));
        var entry = new EntrySpot(new EntrySpotContext("entry"));
        var actor = new PlayerActor("player-1");

        await new RoomPacketHandler().HandleAsync(room, new RoomEvent("opened"), CancellationToken.None);
        var roomReply =
            await new RoomRequestHandler().HandleAsync(room, new JoinRoom("room-1"), CancellationToken.None);
        await new RoomEventHandler().HandleAsync(
            room, new RoomEvent("opened"), (ZLinkPublishMessageContext)null!, CancellationToken.None);
        await new RoomTimerHandler().HandleAsync(room, TimerTick(), CancellationToken.None);
        var joinReply =
            await room.OnActorJoinAsync(Admission(actor), ZLinkMessage.From(new JoinRoom("room-1")), CancellationToken.None);
        await new PlayerActorSendHandler().HandleAsync(room, actor, null!, new RoomEvent("opened"),
            CancellationToken.None);
        var actorReply =
            await new PlayerActorRequestHandler().HandleAsync(room, actor, null!, new JoinRoom("room-1"),
                CancellationToken.None);
        await room.OnJoinedActorAsync(actor, CancellationToken.None);
        await room.OnLeaveActorAsync(actor, CancellationToken.None);
        await room.OnDisconnectActorAsync(actor, CancellationToken.None);
        await new EntryActorSendHandler().HandleAsync(entry, actor, null!, new RoomEvent("opened"),
            CancellationToken.None);
        var entryReply = await new EntryActorRequestHandler().HandleAsync(entry, actor, null!, new JoinRoom("room-1"),
            CancellationToken.None);
        await ((IZLinkEntrySpot<PlayerActor>)entry).OnCreateActorAsync(actor, ZLinkMessage.Empty,
            CancellationToken.None);
        await entry.OnJoinedActorAsync(actor, CancellationToken.None);
        await entry.OnLeaveActorAsync(actor, CancellationToken.None);
        await entry.OnDisconnectActorAsync(actor, CancellationToken.None);

        Assert.Equal("room-1", roomReply.RoomId);
        Assert.True(joinReply.Accepted);
        Assert.Equal("room-1", joinReply.Reply!.Decode<JoinedRoom>().RoomId);
        Assert.Equal("actor:room-1", actorReply.RoomId);
        Assert.Equal("entry:room-1", entryReply.RoomId);
    }

    [Fact]
    [ContractExample(
        typeof(IZLinkSpotActorSendHandler<,,>),
        typeof(IZLinkSpotActorRequestHandler<,,,>),
        typeof(IZLinkEntrySpotActorSendHandler<,,>),
        typeof(IZLinkEntrySpotActorRequestHandler<,,,>))]
    public void Spot_actor_handlers_receive_context_before_payload()
    {
        AssertHandlerParameters(
            typeof(IZLinkSpotActorSendHandler<RoomSpot, PlayerActor, RoomEvent>),
            typeof(RoomSpot),
            typeof(PlayerActor),
            typeof(IZLinkMessageContext),
            typeof(RoomEvent),
            typeof(CancellationToken));
        AssertHandlerParameters(
            typeof(IZLinkSpotActorRequestHandler<RoomSpot, PlayerActor, JoinRoom, JoinedRoom>),
            typeof(RoomSpot),
            typeof(PlayerActor),
            typeof(IZLinkMessageContext),
            typeof(JoinRoom),
            typeof(CancellationToken));
        AssertHandlerParameters(
            typeof(IZLinkEntrySpotActorSendHandler<EntrySpot, PlayerActor, RoomEvent>),
            typeof(EntrySpot),
            typeof(PlayerActor),
            typeof(IZLinkMessageContext),
            typeof(RoomEvent),
            typeof(CancellationToken));
        AssertHandlerParameters(
            typeof(IZLinkEntrySpotActorRequestHandler<EntrySpot, PlayerActor, JoinRoom, JoinedRoom>),
            typeof(EntrySpot),
            typeof(PlayerActor),
            typeof(IZLinkMessageContext),
            typeof(JoinRoom),
            typeof(CancellationToken));
    }

    [Fact]
    public async Task Spot_actor_join_callback_uses_framework_message_and_explicit_acceptance()
    {
        var room = new RoomSpot(new SpotContext("room-1"));
        var actor = new PlayerActor("player-1");
        var request = ZLinkMessage.From(new JoinRoom("room-1"));

        var accepted = await room.OnActorJoinAsync(Admission(actor), request, CancellationToken.None);
        var rejected = ZLinkSpotActorJoinResult.Reject();

        Assert.True(accepted.Accepted);
        Assert.NotNull(accepted.Reply);
        Assert.Equal("room-1", accepted.Reply!.Decode<JoinedRoom>().RoomId);
        Assert.False(rejected.Accepted);
        Assert.Null(rejected.Reply);
    }

    [Fact]
    public void Spot_actor_contexts_expose_only_dispatch_metadata()
    {
        // Spot and Actor handlers all receive the one IZLinkMessageContext declared by
        // 04-channel-messaging; the separate send/request context types are gone.
        AssertContextProperties<IZLinkMessageContext>(
            nameof(IZLinkMessageContext.ChannelName),
            nameof(IZLinkMessageContext.ContentType),
            nameof(IZLinkMessageContext.CorrelationId),
            nameof(IZLinkMessageContext.MeshName),
            nameof(IZLinkMessageContext.Metadata),
            nameof(IZLinkMessageContext.PacketName));

        AssertContextProperties<ZLinkRouteMessageContext>(
            nameof(ZLinkRouteMessageContext.ChannelName),
            nameof(ZLinkRouteMessageContext.ContentType),
            nameof(ZLinkRouteMessageContext.CorrelationId),
            nameof(ZLinkRouteMessageContext.MeshName),
            nameof(ZLinkRouteMessageContext.Metadata),
            nameof(ZLinkRouteMessageContext.PacketName),
            nameof(ZLinkRouteMessageContext.SourceNodeRid));

        AssertContextProperties<ZLinkPublishMessageContext>(
            nameof(ZLinkPublishMessageContext.ChannelName),
            nameof(ZLinkPublishMessageContext.ContentType),
            nameof(ZLinkPublishMessageContext.CorrelationId),
            nameof(ZLinkPublishMessageContext.MeshName),
            nameof(ZLinkPublishMessageContext.Metadata),
            nameof(ZLinkPublishMessageContext.PacketName),
            nameof(ZLinkPublishMessageContext.Source),
            nameof(ZLinkPublishMessageContext.Topic));

        Assert.Null(typeof(IZLinkMessageContext).GetProperty("ActorId"));
        Assert.Null(typeof(IZLinkMessageContext).GetProperty("BoundSession"));
        Assert.Null(typeof(IZLinkMessageContext).GetProperty("Deadline"));
        Assert.Null(typeof(IZLinkMessageContext).GetProperty("Reply"));
        Assert.Null(typeof(IZLinkMessageContext).GetProperty("ConnectionAborted"));
    }

    private static void AssertHandlerParameters(Type handlerType, params Type[] expected)
    {
        var actual = handlerType.GetMethod("HandleAsync")!
            .GetParameters()
            .Select(static parameter => parameter.ParameterType)
            .ToArray();

        Assert.Equal(expected, actual);
    }

    private static void AssertContextProperties<TContext>(params string[] expected)
    {
        var actual = typeof(TContext)
            .GetProperties()
            .Select(static property => property.Name)
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Equal(expected.Order(StringComparer.Ordinal).ToArray(), actual);
    }

    private static ZLinkTimerTick TimerTick()
    {
        return new ZLinkTimerTick(
            "heartbeat",
            1,
            1,
            TimeSpan.FromSeconds(1),
            DateTimeOffset.UtcNow,
            DateTimeOffset.UtcNow,
            TimeSpan.FromSeconds(1),
            TimeSpan.FromSeconds(1),
            TimeSpan.Zero,
            0);
    }

    private static string Admission(PlayerActor actor) => actor.Context.ActorId;

    private sealed record JoinRoom(string RoomId);

    private sealed record JoinedRoom(string RoomId);

    private sealed record RoomEvent(string State);

    private sealed class RoomSpot(IZLinkSpotContext context) : IZLinkSpot<PlayerActor>
    {
        public IZLinkSpotContext Context { get; } = context;

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            _ = actorId;
            _ = cancellationToken;
            var join = request.Decode<JoinRoom>();
            return ValueTask.FromResult(
                ZLinkSpotActorJoinResult.Accept(new JoinedRoom(join.RoomId)));
        }

        public ValueTask OnJoinedActorAsync(
            PlayerActor actor,
            CancellationToken cancellationToken)
        {
            _ = actor;
            _ = cancellationToken;
            return ValueTask.CompletedTask;
        }

        public ValueTask OnLeaveActorAsync(
            PlayerActor actor,
            CancellationToken cancellationToken)
        {
            _ = actor;
            _ = cancellationToken;
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDisconnectActorAsync(
            PlayerActor actor,
            CancellationToken cancellationToken)
        {
            _ = actor;
            _ = cancellationToken;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class EntrySpot(IZLinkEntrySpotContext context) : IZLinkEntrySpot<PlayerActor>
    {
        public IZLinkEntrySpotContext Context { get; } = context;

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            _ = actorId;
            _ = cancellationToken;
            return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));
        }

        public ValueTask OnJoinedActorAsync(
            PlayerActor actor,
            CancellationToken cancellationToken)
        {
            _ = actor;
            _ = cancellationToken;
            return ValueTask.CompletedTask;
        }

        public ValueTask OnLeaveActorAsync(
            PlayerActor actor,
            CancellationToken cancellationToken)
        {
            _ = actor;
            _ = cancellationToken;
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDisconnectActorAsync(
            PlayerActor actor,
            CancellationToken cancellationToken)
        {
            _ = actor;
            _ = cancellationToken;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class PlayerActor(string actorId) : IZLinkActor
    {
        public IZLinkActorContext Context { get; } = new TestActorContext(actorId);
    }

    private sealed class TestActorContext(string actorId) : IZLinkActorContext
    {
        public string ActorId => actorId;

        public ulong ObjectGeneration => 1;

        public string MeshName => "play";

        public string? SpotId => "room-1";

        public IZLinkBoundSession BoundSession => null!;

        public IZLinkActorJoinSpotCall JoinSpot(string spotId, ZLinkMessage request) => null!;

        public IZLinkActorJoinEntrySpotCall JoinEntrySpot(ZLinkMessage request) => null!;
    }

    private sealed class SpotContext(string spotId) :
        IZLinkSpotContext,
        IZLinkSpotHandlerRegistry,
        IZLinkSpotOutbound
    {
        public string MeshName => "play";

        public List<string> JoinedActors { get; } = [];

        public List<string> LeftActors { get; } = [];

        public List<string> Timers { get; } = [];
        public string SpotId { get; } = spotId;

        public ulong ObjectGeneration => 1;

        public RoutingId NodeRid => RoutingId.From("spot-node");

        public IZLinkSpotHandlerRegistry Handlers => this;

        public IZLinkSpotOutbound Outbound => this;

        public IZLinkSpotRelocationReadyCall RelocationReady() =>
            new RelocationReadyCall();

        public ValueTask LeaveActorAsync(
            IZLinkActor actor,
            CancellationToken cancellationToken = default)
        {
            LeftActors.Add(actor.Context.ActorId);
            return ValueTask.CompletedTask;
        }

        public ValueTask<bool> CloseAsync(CancellationToken cancellationToken = default)
        {
            return ValueTask.FromResult(true);
        }

        public ValueTask<IZLinkTimer> AddTimer<THandler>(
            string name,
            TimeSpan period,
            ZLinkTimerOptions? options = null,
            CancellationToken cancellationToken = default)
            where THandler : class
        {
            Timers.Add(name);
            return ValueTask.FromResult<IZLinkTimer>(new Timer());
        }

        public IZLinkWorkerCall<TResult> RunCpuWorker<TResult>(
            Func<CancellationToken, TResult> work)
        {
            return new WorkerCall<TResult>(work);
        }

        public IZLinkWorkerCall<TResult> RunIoWorker<TResult>(
            Func<CancellationToken, ValueTask<TResult>> work)
        {
            return new IoWorkerCall<TResult>(work);
        }

        private sealed class RelocationReadyCall
            : IZLinkSpotRelocationReadyCall
        {
            public void Defer()
            {
            }
        }

        public void AddHandler<THandler>()
            where THandler : class
        {
        }

        public void AddHandler<THandler>(string packetName)
            where THandler : class
        {
        }

        public void AddActorPacket<THandler, TActor>()
            where THandler : class
            where TActor : IZLinkActor
        {
        }

        public void AddActorPacket<THandler, TActor>(string packetName)
            where THandler : class
            where TActor : IZLinkActor
        {
        }

        public void AddPacket<THandler>()
            where THandler : class
        {
        }

        public void AddSubscribe<THandler>(string channelName, string topic)
            where THandler : class
        {
        }

        public IZLinkSpotSendCall SendToSpot<TMessage>(string spotId, TMessage message) =>
            new SpotSendCall();

        public IZLinkSpotRequestCall RequestToSpot<TMessage>(string spotId, TMessage request) =>
            new SpotRequestCall(new JoinedRoom("room-1"));

        public IZLinkPublishCall Publish<TEvent>(string channelName, string topic, TEvent message)
        {
            return new PublishCall();
        }

        public IZLinkSendCall SendToChannel<TMessage>(string channelName, TMessage message)
        {
            return new SendCall();
        }

        public IZLinkRequestCall RequestToChannel<TRequest>(string channelName, TRequest request)
        {
            return new RequestCall(new JoinedRoom("room-1"));
        }
    }

    private sealed class EntrySpotContext(string spotId) :
        IZLinkEntrySpotContext,
        IZLinkSpotHandlerRegistry,
        IZLinkSpotOutbound
    {
        public string MeshName => "play";

        public List<string> DestroyedActors { get; } = [];
        public string SpotId { get; } = spotId;

        public ulong ObjectGeneration => 1;

        public RoutingId NodeRid => RoutingId.From("spot-node");

        public IZLinkSpotHandlerRegistry Handlers => this;

        public IZLinkSpotOutbound Outbound => this;

        public ValueTask DestroyActorAsync(
            IZLinkActor actor,
            CancellationToken cancellationToken = default)
        {
            DestroyedActors.Add(actor.Context.ActorId);
            return ValueTask.CompletedTask;
        }

        public ValueTask<IZLinkTimer> AddTimer<THandler>(
            string name,
            TimeSpan period,
            ZLinkTimerOptions? options = null,
            CancellationToken cancellationToken = default)
            where THandler : class
        {
            return ValueTask.FromResult<IZLinkTimer>(new Timer());
        }

        public IZLinkWorkerCall<TResult> RunCpuWorker<TResult>(
            Func<CancellationToken, TResult> work)
        {
            return new WorkerCall<TResult>(work);
        }

        public IZLinkWorkerCall<TResult> RunIoWorker<TResult>(
            Func<CancellationToken, ValueTask<TResult>> work)
        {
            return new IoWorkerCall<TResult>(work);
        }

        public void AddHandler<THandler>()
            where THandler : class
        {
        }

        public void AddHandler<THandler>(string packetName)
            where THandler : class
        {
        }

        public void AddActorPacket<THandler, TActor>()
            where THandler : class
            where TActor : IZLinkActor
        {
        }

        public void AddActorPacket<THandler, TActor>(string packetName)
            where THandler : class
            where TActor : IZLinkActor
        {
        }

        public void AddPacket<THandler>()
            where THandler : class
        {
        }

        public void AddSubscribe<THandler>(string channelName, string topic)
            where THandler : class
        {
        }

        public IZLinkSpotSendCall SendToSpot<TMessage>(string spotId, TMessage message) =>
            new SpotSendCall();

        public IZLinkSpotRequestCall RequestToSpot<TMessage>(string spotId, TMessage request) =>
            new SpotRequestCall();

        public IZLinkPublishCall Publish<TEvent>(string channelName, string topic, TEvent message)
        {
            return new PublishCall();
        }

        public IZLinkSendCall SendToChannel<TMessage>(string channelName, TMessage message)
        {
            return new SendCall();
        }

        public IZLinkRequestCall RequestToChannel<TRequest>(string channelName, TRequest request)
        {
            return new RequestCall(new JoinedRoom("room-1"));
        }
    }

    private sealed class WorkerCall<TResult>(Func<CancellationToken, TResult> work) : IZLinkWorkerCall<TResult>
    {
        public void Submit(CancellationToken cancellationToken = default)
        {
            _ = work(cancellationToken);
        }

        public IZLinkWorkerCall<TResult> Timeout(TimeSpan timeout)
        {
            return this;
        }

        public ValueTask<TResult> Async(CancellationToken cancellationToken = default)
        {
            return ValueTask.FromResult(work(cancellationToken));
        }

        public ValueTask<TResult> Yield(CancellationToken cancellationToken = default)
        {
            return Async(cancellationToken);
        }

    }

    private sealed class IoWorkerCall<TResult>(
        Func<CancellationToken, ValueTask<TResult>> work) : IZLinkWorkerCall<TResult>
    {
        public void Submit(CancellationToken cancellationToken = default)
        {
            _ = work(cancellationToken);
        }

        public IZLinkWorkerCall<TResult> Timeout(TimeSpan timeout)
        {
            return this;
        }

        public ValueTask<TResult> Async(CancellationToken cancellationToken = default)
        {
            return work(cancellationToken);
        }

        public ValueTask<TResult> Yield(CancellationToken cancellationToken = default)
        {
            return work(cancellationToken);
        }
    }

    private sealed class SpotManager : IZLinkSpotManager
    {
        private readonly Dictionary<string, SpotRef> _spots = [];

        public IZLinkSpotCreateCall Create(string spotType) =>
            new SpotCreateCall(this, $"{spotType}-1");

        public IZLinkSpotGetOrCreateCall GetOrCreate(string spotId, string spotType) =>
            new SpotGetOrCreateCall(this, spotId);

        public ValueTask<SpotRef?> FindAsync(string spotId,
            CancellationToken cancellationToken = default)
        {
            return ValueTask.FromResult<SpotRef?>(
                _spots.TryGetValue(spotId, out var spot) ? spot : null);
        }

        public ValueTask<bool> CloseAsync(
            SpotRef spot,
            CancellationToken cancellationToken = default)
        {
            return ValueTask.FromResult(_spots.Remove(spot.SpotId));
        }

        private ZLinkSpotCreateResult Submit(string spotId)
        {
            var created = !_spots.ContainsKey(spotId);
            var spot = new SpotRef(
                spotId,
                1,
                "play",
                RoutingId.From("spot-node"));
            _spots[spotId] = spot;
            return new ZLinkSpotCreateResult(
                spot,
                created ? ZLinkSpotCreateState.Created : ZLinkSpotCreateState.Existing,
                null);
        }

        private abstract class SpotCall(SpotManager manager, string spotId)
        {
            protected ValueTask<ZLinkSpotCreateResult> SubmitAsync() =>
                ValueTask.FromResult(manager.Submit(spotId));
        }

        private sealed class SpotCreateCall(
            SpotManager manager,
            string spotId) : SpotCall(manager, spotId), IZLinkSpotCreateCall
        {
            public IZLinkSpotCreateCall InMesh(string meshName) => this;
            public IZLinkSpotCreateCall Request(ZLinkMessage request) => this;
            public IZLinkSpotCreateCall Request<TRequest>(TRequest request) => this;
            public IZLinkSpotCreateCall Timeout(TimeSpan timeout) => this;
            public ValueTask<ZLinkSpotCreateResult> Async(
                CancellationToken cancellationToken = default) => SubmitAsync();
            public ValueTask<ZLinkSpotCreateResult> Yield(
                CancellationToken cancellationToken = default) => SubmitAsync();
        }

        private sealed class SpotGetOrCreateCall(
            SpotManager manager,
            string spotId) : SpotCall(manager, spotId), IZLinkSpotGetOrCreateCall
        {
            public IZLinkSpotGetOrCreateCall InMesh(string meshName) => this;
            public IZLinkSpotGetOrCreateCall Request(ZLinkMessage request) => this;
            public IZLinkSpotGetOrCreateCall Request<TRequest>(TRequest request) => this;
            public IZLinkSpotGetOrCreateCall Timeout(TimeSpan timeout) => this;
            public ValueTask<ZLinkSpotCreateResult> Async(
                CancellationToken cancellationToken = default) => SubmitAsync();
            public ValueTask<ZLinkSpotCreateResult> Yield(
                CancellationToken cancellationToken = default) => SubmitAsync();
        }
    }

    private sealed class SpotOutbound : IZLinkSpotOutbound, IZLinkSpotClient
    {
        public IZLinkSpotSendCall SendToSpot<TMessage>(string spotId, TMessage message) =>
            new SpotSendCall();

        public IZLinkSpotRequestCall RequestToSpot<TMessage>(string spotId, TMessage request) =>
            new SpotRequestCall(new JoinedRoom("room-1"));

        public IZLinkPublishCall Publish<TEvent>(string channelName, string topic, TEvent message)
        {
            return new PublishCall();
        }

        public IZLinkSendCall SendToChannel<TMessage>(string channelName, TMessage message)
        {
            return new SendCall();
        }

        public IZLinkRequestCall RequestToChannel<TMessage>(string channelName, TMessage request)
        {
            return new RequestCall(new JoinedRoom("room-1"));
        }
    }

    private sealed class SpotPublisherClient : IZLinkSpotPublisherClient
    {
        public IZLinkPublishCall Publish<TEvent>(
            string channelName,
            string topic,
            TEvent message)
        {
            return new PublishCall();
        }
    }

    private sealed class SendCall : IZLinkSendCall
    {
        public IZLinkSendCall Metadata(string key, string value) => this;

        public IZLinkSendCall Metadata(ZLinkMessageMetadata metadata) => this;

        public ValueTask Async(CancellationToken cancellationToken = default) =>
            ValueTask.CompletedTask;
    }

    private sealed class SpotSendCall : IZLinkSpotSendCall
    {
        public IZLinkSpotSendCall InstanceSpot() => this;
        public IZLinkSpotSendCall InstanceSpot(string instanceSpotType) => this;
        public IZLinkSpotSendCall InMesh(string meshName) => this;
        public IZLinkSpotSendCall Metadata(string key, string value) => this;
        public IZLinkSpotSendCall Metadata(ZLinkMessageMetadata metadata) => this;
        public ValueTask Async(CancellationToken cancellationToken = default) =>
            ValueTask.CompletedTask;
    }

    private sealed class SpotRequestCall(object? reply = null) : IZLinkSpotRequestCall
    {
        public IZLinkSpotRequestCall InstanceSpot() => this;
        public IZLinkSpotRequestCall InstanceSpot(string instanceSpotType) => this;
        public IZLinkSpotRequestCall InMesh(string meshName) => this;
        public IZLinkSpotRequestCall Metadata(string key, string value) => this;
        public IZLinkSpotRequestCall Metadata(ZLinkMessageMetadata metadata) => this;
        public IZLinkSpotRequestCall Timeout(TimeSpan timeout) => this;
        public ValueTask<TReply> Async<TReply>(CancellationToken cancellationToken = default) =>
            ValueTask.FromResult((TReply)reply!);
        public ValueTask<TReply> Yield<TReply>(CancellationToken cancellationToken = default) =>
            Async<TReply>(cancellationToken);
    }

    private sealed class RequestCall(object reply) : IZLinkRequestCall
    {
        public IZLinkRequestCall Metadata(string key, string value) => this;

        public IZLinkRequestCall Metadata(ZLinkMessageMetadata metadata) => this;

        public IZLinkRequestCall Timeout(TimeSpan timeout)
        {
            return this;
        }

        public ValueTask<TReply> Async<TReply>(CancellationToken cancellationToken = default)
        {
            return ValueTask.FromResult((TReply)reply);
        }

        public ValueTask<TReply> Yield<TReply>(CancellationToken cancellationToken = default)
        {
            return Async<TReply>(cancellationToken);
        }

    }

    private sealed class PublishCall : IZLinkPublishCall
    {
        public IZLinkPublishCall Metadata(string key, string value) => this;

        public IZLinkPublishCall Metadata(ZLinkMessageMetadata metadata) => this;

        public ValueTask Async(CancellationToken cancellationToken = default) =>
            ValueTask.CompletedTask;
    }

    private sealed class Timer : IZLinkTimer
    {
        public bool IsDisposed { get; private set; }

        public ValueTask CancelAsync()
        {
            IsDisposed = true;
            return ValueTask.CompletedTask;
        }

        public ValueTask DisposeAsync()
        {
            IsDisposed = true;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class RoomPacketHandler : IZLinkSpotPacketHandler<RoomSpot, RoomEvent>
    {
        public ValueTask HandleAsync(
            RoomSpot spot,
            RoomEvent message,
            CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }
    }

    private sealed class RoomRequestHandler : IZLinkSpotRequestHandler<RoomSpot, JoinRoom, JoinedRoom>
    {
        public ValueTask<JoinedRoom> HandleAsync(RoomSpot spot, JoinRoom request, CancellationToken cancellationToken)
        {
            return ValueTask.FromResult(new JoinedRoom(request.RoomId));
        }
    }

    private sealed class RoomEventHandler : IZLinkSpotSubscriptionHandler<RoomSpot, RoomEvent>
    {
        public ValueTask HandleAsync(
            RoomSpot spot,
            RoomEvent message,
            ZLinkPublishMessageContext context,
            CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }
    }

    private sealed class RoomTimerHandler : IZLinkSpotTimerHandler<RoomSpot>
    {
        public ValueTask HandleAsync(RoomSpot spot, ZLinkTimerTick tick, CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }
    }

    private sealed class PlayerActorPacketHandler : IZLinkSpotActorSendHandler<RoomSpot, PlayerActor, RoomEvent>
    {
        public ValueTask HandleAsync(
            RoomSpot spot,
            PlayerActor actor,
            IZLinkMessageContext context,
            RoomEvent message,
            CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }
    }

    private sealed class PlayerActorSendHandler : IZLinkSpotActorSendHandler<RoomSpot, PlayerActor, RoomEvent>
    {
        public ValueTask HandleAsync(
            RoomSpot spot,
            PlayerActor actor,
            IZLinkMessageContext context,
            RoomEvent message,
            CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }
    }

    private sealed class
        PlayerActorRequestHandler : IZLinkSpotActorRequestHandler<RoomSpot, PlayerActor, JoinRoom, JoinedRoom>
    {
        public ValueTask<JoinedRoom> HandleAsync(
            RoomSpot spot,
            PlayerActor actor,
            IZLinkMessageContext context,
            JoinRoom request,
            CancellationToken cancellationToken)
        {
            return ValueTask.FromResult(new JoinedRoom($"actor:{request.RoomId}"));
        }
    }

    private sealed class EntryActorSendHandler : IZLinkEntrySpotActorSendHandler<EntrySpot, PlayerActor, RoomEvent>
    {
        public ValueTask HandleAsync(
            EntrySpot entrySpot,
            PlayerActor actor,
            IZLinkMessageContext context,
            RoomEvent message,
            CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }
    }

    private sealed class
        EntryActorRequestHandler : IZLinkEntrySpotActorRequestHandler<EntrySpot, PlayerActor, JoinRoom, JoinedRoom>
    {
        public ValueTask<JoinedRoom> HandleAsync(
            EntrySpot entrySpot,
            PlayerActor actor,
            IZLinkMessageContext context,
            JoinRoom request,
            CancellationToken cancellationToken)
        {
            return ValueTask.FromResult(new JoinedRoom($"entry:{request.RoomId}"));
        }
    }
}
