using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Actors;

public sealed class ActorContracts
{
    [Fact]
    public void Actor_join_completion_failed_shape_matches_exact_interface()
    {
        var failed = typeof(ZLinkActorJoinCompletion.Failed);
        var constructor = Assert.Single(failed.GetConstructors());

        Assert.Equal(
            new[] { "Kind", "OperationId" },
            failed.GetProperties()
                .Select(static property => property.Name)
                .Order(StringComparer.Ordinal)
                .ToArray());
        Assert.Equal(
            new[] { typeof(ZLinkActorJoinOperationId), typeof(ZLinkFrameworkErrorKind) },
            constructor.GetParameters()
                .Select(static parameter => parameter.ParameterType)
                .ToArray());
        Assert.DoesNotContain(
            failed.GetMembers(),
            static member => member.Name == "IsRetriable");
    }

    [Fact]
    public void Actor_ref_public_shape_is_exact()
    {
        var propertyNames = typeof(ActorRef)
            .GetProperties()
            .Select(static property => property.Name)
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Equal(
            new[] { "ActorId", "MeshName", "NodeRid", "ObjectGeneration" },
            propertyNames);
        Assert.DoesNotContain(
            typeof(ActorRef).GetProperties(),
            static property => property.Name == "Generation");
        Assert.Null(
            typeof(IZLinkActor).Assembly.GetType(
                "Zlink.Framework.Contracts.Actors.ActorRef",
                throwOnError: false));

        var actorClientMethods = typeof(IZLinkActorClient)
            .GetMethods()
            .OrderBy(static method => method.Name, StringComparer.Ordinal)
            .ToArray();
        Assert.Equal(2, actorClientMethods.Length);
        Assert.All(actorClientMethods, static method =>
        {
            var parameters = method.GetParameters();
            Assert.Equal("actorId", parameters[0].Name);
            Assert.Equal(typeof(string), parameters[0].ParameterType);
            Assert.DoesNotContain(
                parameters,
                static parameter => parameter.ParameterType == typeof(ActorRef));
        });
    }

    [Fact]
    public void Actor_ref_validation_is_preserved_by_the_positional_projection()
    {
        Assert.Throws<ArgumentException>(() => new ActorRef(
            string.Empty,
            1,
            "mesh",
            RoutingId.From("node")));
        Assert.Throws<ArgumentOutOfRangeException>(() => new ActorRef(
            "actor",
            0,
            "mesh",
            RoutingId.From("node")));

        var actor = new ActorRef("actor", 1, "mesh", RoutingId.From("node"));
        Assert.Throws<ArgumentException>(() => actor with { ActorId = string.Empty });
        Assert.Throws<ArgumentOutOfRangeException>(() => actor with { ObjectGeneration = 0 });
        Assert.Throws<ArgumentException>(() => actor with { MeshName = string.Empty });
        Assert.Throws<ArgumentException>(() => actor with { NodeRid = default });
    }

    [Fact]
    [ContractExample(
        typeof(IZLinkActor),
        typeof(IZLinkActorContext),
        typeof(IZLinkActorDeferredJoinCall),
        typeof(ZLinkActorJoinCompletion),
        typeof(IZLinkActorJoinSpotCall),
        typeof(IZLinkActorJoinEntrySpotCall),
        typeof(IZLinkActorClient),
        typeof(IZLinkActorSendCall),
        typeof(IZLinkActorRequestCall),
        typeof(IZLinkActorFactory),
        typeof(IZLinkActorManager),
        typeof(IZLinkActorCreateCall),
        typeof(IZLinkActorGetOrCreateCall),
        typeof(ZLinkActorCreateResult))]
    public async Task Actor_context_creates_actors_and_joins_a_spot_by_spot_id()
    {
        var spot = new RoomSpot();
        var context = new ActorContext("player-1");
        var factory = new ActorFactory();
        var manager = new ActorManager(factory, context);
        var actorClient = new ActorClient();

        var actor = new PlayerActor(context);
        var actorRef = (await manager.GetOrCreate("player-1", "player")
            .InMesh("actors")
            .Request(new JoinRoom("room-1"))
            .Timeout(TimeSpan.FromSeconds(1))
            .Async()) switch
        {
            ZLinkActorCreateResult.Existing value => value.Actor,
            ZLinkActorCreateResult.Created value => value.Actor,
            _ => throw new InvalidOperationException("Actor creation was rejected.")
        };
        var foundActorRef = await manager.FindAsync("player-1");
        actor.Context
            .JoinSpot("room-1", new JoinRoom("room-1"))
            .Defer();
        actor.Context
            .JoinEntrySpot(ZLinkMessage.Empty)
            .Timeout(TimeSpan.FromSeconds(1))
            .Defer();
        await actorClient.SendToActor(actorRef.ActorId, new JoinRoom("room-1"))
            .Async();
        var actorReply = await actorClient
            .RequestToActor(actorRef.ActorId, new JoinRoom("room-1"))
            .Timeout(TimeSpan.FromSeconds(1))
            .Async<JoinedRoom>();
        var yieldedActorReply = await actorClient
            .RequestToActor(actorRef.ActorId, new JoinRoom("room-1"))
            .Timeout(TimeSpan.FromSeconds(1))
            .Yield<JoinedRoom>();

        Assert.Equal("player-1", actorRef.ActorId);
        Assert.Equal(actorRef, foundActorRef);
        Assert.Equal("player-1", actor.Context.ActorId);
        Assert.Equal("room-1", actorReply.RoomId);
        Assert.Equal("room-1", yieldedActorReply.RoomId);
    }

    private sealed class ActorClient : IZLinkActorClient
    {
        public IZLinkActorSendCall SendToActor<TMessage>(
            string actorId,
            TMessage message)
        {
            _ = actorId;
            _ = message;
            return new ActorSendCall();
        }

        public IZLinkActorRequestCall RequestToActor<TRequest>(
            string actorId,
            TRequest request)
        {
            _ = actorId;
            _ = request;
            return new ActorRequestCall();
        }
    }

    private sealed class ActorSendCall : IZLinkActorSendCall
    {
        public IZLinkActorSendCall Metadata(string key, string value) => this;

        public IZLinkActorSendCall Metadata(ZLinkMessageMetadata metadata) => this;

        public ValueTask Async(
            CancellationToken cancellationToken = default) =>
            ValueTask.CompletedTask;
    }

    private sealed class ActorRequestCall : IZLinkActorRequestCall
    {
        public IZLinkActorRequestCall Metadata(string key, string value) => this;

        public IZLinkActorRequestCall Metadata(ZLinkMessageMetadata metadata) => this;

        public IZLinkActorRequestCall Timeout(TimeSpan timeout)
        {
            _ = timeout;
            return this;
        }

        public ValueTask<TReply> Async<TReply>(CancellationToken cancellationToken = default)
        {
            object reply = new JoinedRoom("room-1");
            return ValueTask.FromResult((TReply)reply);
        }

        public ValueTask<TReply> Yield<TReply>(CancellationToken cancellationToken = default)
        {
            return Async<TReply>(cancellationToken);
        }
    }

    private sealed record JoinRoom(string RoomId);

    private sealed record JoinedRoom(string RoomId);

    private sealed class ActorFactory : IZLinkActorFactory
    {
        public ValueTask<IZLinkActor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken cancellationToken = default)
        {
            return ValueTask.FromResult<IZLinkActor>(new PlayerActor(context));
        }
    }

    private sealed class ActorManager(IZLinkActorFactory factory, IZLinkActorContext context) : IZLinkActorManager
    {
        private readonly Dictionary<string, ActorRef> _actors = [];

        public IZLinkActorCreateCall Create(string actorId, string actorType) =>
            new ActorCreateCall(this, actorId, actorType, getOrCreate: false);

        public IZLinkActorGetOrCreateCall GetOrCreate(string actorId, string actorType) =>
            new ActorCreateCall(this, actorId, actorType, getOrCreate: true);

        public ValueTask<ActorRef?> FindAsync(
            string actorId,
            CancellationToken cancellationToken = default)
        {
            return ValueTask.FromResult<ActorRef?>(
                _actors.TryGetValue(actorId, out var actorRef)
                    ? actorRef
                    : null);
        }

        public ValueTask<SpotRef?> FindSpotAsync(
            string actorId,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult<SpotRef?>(null);

        public ValueTask<bool> DestroyAsync(
            ActorRef actor,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(_actors.Remove(actor.ActorId));

        private async ValueTask<ZLinkActorCreateResult> SubmitAsync(
            string actorId,
            string actorType,
            bool getOrCreate,
            CancellationToken cancellationToken)
        {
            if (getOrCreate && await FindAsync(actorId, cancellationToken) is { } existing)
                return new ZLinkActorCreateResult.Existing(existing);

            _ = actorType;
            var actor = await factory.CreateAsync(context, cancellationToken);
            var actorRef = new ActorRef(
                actor.Context.ActorId,
                1,
                "actors",
                RoutingId.From("actor-node"));
            _actors[actorId] = actorRef;
            return new ZLinkActorCreateResult.Created(actorRef, null);
        }

        private sealed class ActorCreateCall(
            ActorManager manager,
            string actorId,
            string actorType,
            bool getOrCreate) : IZLinkActorCreateCall, IZLinkActorGetOrCreateCall
        {
            public IZLinkActorCreateCall InMesh(string meshName)
            {
                _ = meshName;
                return this;
            }

            IZLinkActorGetOrCreateCall IZLinkActorGetOrCreateCall.InMesh(string meshName)
            {
                _ = meshName;
                return this;
            }

            public IZLinkActorCreateCall Request(ZLinkMessage request)
            {
                _ = request;
                return this;
            }

            IZLinkActorGetOrCreateCall IZLinkActorGetOrCreateCall.Request(ZLinkMessage request)
            {
                _ = request;
                return this;
            }

            public IZLinkActorCreateCall Request<TRequest>(TRequest request)
            {
                _ = request;
                return this;
            }

            IZLinkActorGetOrCreateCall IZLinkActorGetOrCreateCall.Request<TRequest>(TRequest request)
            {
                _ = request;
                return this;
            }

            public IZLinkActorCreateCall Timeout(TimeSpan timeout)
            {
                _ = timeout;
                return this;
            }

            IZLinkActorGetOrCreateCall IZLinkActorGetOrCreateCall.Timeout(TimeSpan timeout)
            {
                _ = timeout;
                return this;
            }

            public ValueTask<ZLinkActorCreateResult> Async(
                CancellationToken cancellationToken = default) =>
                manager.SubmitAsync(actorId, actorType, getOrCreate, cancellationToken);

            public ValueTask<ZLinkActorCreateResult> Yield(
                CancellationToken cancellationToken = default) =>
                manager.SubmitAsync(actorId, actorType, getOrCreate, cancellationToken);
        }
    }

    private sealed class ActorContext(string actorId) : IZLinkActorContext
    {
        public string ActorId => actorId;

        public ulong ObjectGeneration => 1;

        public string MeshName => "play";

        public string? SpotId => "room-1";

        public IZLinkBoundSession BoundSession { get; } = new BoundSession();

        public IZLinkActorJoinSpotCall JoinSpot(
            string spotId,
            ZLinkMessage request)
        {
            return new JoinSpotCall();
        }

        public IZLinkActorJoinEntrySpotCall JoinEntrySpot(ZLinkMessage request)
        {
            return new JoinEntrySpotCall();
        }
    }

    private sealed class JoinSpotCall : IZLinkActorJoinSpotCall
    {
        public void Defer()
        {
        }

        public IZLinkActorJoinSpotCall Timeout(TimeSpan timeout)
        {
            return this;
        }

    }

    private sealed class JoinEntrySpotCall : IZLinkActorJoinEntrySpotCall
    {
        public void Defer()
        {
        }

        public IZLinkActorJoinEntrySpotCall Timeout(TimeSpan timeout)
        {
            return this;
        }

    }

    private sealed class PlayerActor(IZLinkActorContext context) : IZLinkActor
    {
        public IZLinkActorContext Context { get; } = context;
    }

    private sealed class RoomSpot : IZLinkSpot
    {
        public IZLinkSpotContext Context => null!;
    }

    private sealed class BoundSession : IZLinkBoundSession
    {
        public IZLinkBoundSessionSendCall Send<TMessage>(TMessage message)
        {
            return null!;
        }

        public ValueTask DisconnectAsync(CancellationToken cancellationToken = default)
        {
            return ValueTask.CompletedTask;
        }
    }
}
