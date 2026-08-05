using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Streams;

public sealed class StreamContracts
{
    [Fact]
    [ContractExample(
        typeof(IZLinkSession),
        typeof(IZLinkSessionContext),
        typeof(IZLinkSessionClient),
        typeof(IZLinkSessionActors),
        typeof(IZLinkSessionSendCall),
        typeof(IZLinkSessionReplyCall),
        typeof(IZLinkSessionHandlerRegistry),
        typeof(IZLinkSessionPacketHandler<,>),
        typeof(IZLinkSessionActor),
        typeof(IZLinkStream))]
    public async Task Session_context_collects_identity_stream_and_actor_operations()
    {
        var context = new ExampleSessionContext();
        var session = new ExampleSession(context);

        await session.OnConnectedAsync(CancellationToken.None);
        var actorRef =
            await context.Actors.BindAsync(new Systems.Zlink.ActorRef(
                "player-1",
                1,
                "actors",
                RoutingId.From("actor-node")));
        var sameActorRef =
            await context.Actors.BindOrGetAsync(new Systems.Zlink.ActorRef(
                "player-1",
                1,
                "actors",
                RoutingId.From("actor-node")));
        var boundActor = context.Actors.Find("player-1");
        await actorRef.RelayAsync(
            ZLinkMessage.From(new PlayerJoined("player-1")));
        await actorRef.NotifyDisconnectedAsync();

        await context.Client.Send(new PlayerJoined("player-1"))
            .Metadata("trace-id", "abc")
            .Compress()
            .Async();

        await context.Client.Reply(new AuthenticateReply("player-1"))
            .Compress()
            .Async();

        await context.CloseAsync();

        IZLinkStream stream = context;
        stream.Write(ZLinkMessage.From(new AuthenticateReply("token")));
        await stream.CloseAsync();

        Assert.Equal("session-1", session.Context.SessionId);
        Assert.Equal("player-1", actorRef.ActorId);
        Assert.Same(actorRef, sameActorRef);
        Assert.Same(actorRef, boundActor);
        Assert.True(context.IsClosed);
        Assert.True(context.StreamClosed);
    }

    [Fact]
    public void Session_actor_binding_accepts_only_an_exact_actor_ref()
    {
        var bindParameters = typeof(IZLinkSessionActors)
            .GetMethods()
            .Where(static method => method.Name == nameof(IZLinkSessionActors.BindAsync))
            .Select(static method => method.GetParameters()[0].ParameterType)
            .ToArray();

        Assert.Equal(new[] { typeof(Systems.Zlink.ActorRef) }, bindParameters);
        Assert.DoesNotContain(typeof(IZLinkActor), bindParameters);
    }

    [Fact]
    [ContractExample(
        typeof(IZLinkSessionHandlerRegistry),
        typeof(IZLinkSessionPacketHandler<,>))]
    public async Task Session_configure_registers_typed_packet_handlers()
    {
        var sessionContext = new SessionPacketContext();
        IZLinkSessionHandlerRegistry handlers =
            new ExampleSessionHandlerRegistry(sessionContext);

        handlers.AddHandler<AuthenticatePacketHandler>();

        var handled = await handlers.TryHandleAsync(
            new ZLinkSessionDispatchContext(nameof(AuthenticateReply)),
            ZLinkMessage.From(new AuthenticateReply("token")));
        var unhandled = await handlers.TryHandleAsync(
            new ZLinkSessionDispatchContext("gameplay"),
            ZLinkMessage.From(new PlayerJoined("player-1")));

        Assert.True(handled);
        Assert.False(unhandled);
        Assert.Equal(nameof(AuthenticateReply), sessionContext.LastPacketName);
    }

    [Fact]
    [ContractExample(
        typeof(IZLinkBoundSession),
        typeof(IZLinkBoundSessionSendCall),
        typeof(IZLinkMessageMetadataPolicy))]
    public async Task Bound_session_sends_to_the_bound_session_without_exposing_stream_transport()
    {
        var boundSession = new ExampleBoundSession();

        await boundSession.Send(new PlayerJoined("player-1"))
            .Metadata("trace-id", "abc")
            .Async();

        await boundSession.Send(new PlayerJoined("player-2"))
            .Async();

        await boundSession.DisconnectAsync();

        IZLinkMessageMetadataPolicy policy = new MetadataPolicy(
            new HashSet<string>(StringComparer.Ordinal) { "trace-id" });
        var metadata = new ZLinkMessageMetadata(new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["trace-id"] = "abc"
        });

        Assert.True(boundSession.IsDisconnected);
        Assert.True(policy.CanForward("trace-id"));
        Assert.False(policy.CanForward("internal-key"));
        Assert.Equal("abc", metadata.Find("trace-id"));
        Assert.Null(metadata.Find("tenant-id"));
        Assert.Null(typeof(ZLinkMessageMetadata).GetProperty("Application"));
        Assert.Null(typeof(ZLinkMessageMetadata).GetProperty("Codec"));
        Assert.Null(typeof(ZLinkMessageMetadata).GetMethod("TryGetApplicationValue"));
        Assert.Null(typeof(ZLinkMessageMetadata).GetMethod("TryGetCodecValue"));
    }

    private sealed record PlayerJoined(string PlayerId);

    private sealed record AuthenticateReply(string PlayerId);

    private sealed class SessionPacketContext : IZLinkSessionContext
    {
        public string? LastPacketName { get; set; }

        public string SessionId => "session-1";

        public RoutingId? RoutingId => Systems.Zlink.RoutingId.From("session-route");

        public string? LocalAddr => "tcp://127.0.0.1:5000";

        public string? RemoteAddr => "tcp://127.0.0.1:5001";

        public IZLinkSessionClient Client => throw new NotSupportedException();

        public IZLinkSessionActors Actors => throw new NotSupportedException();

        public IZLinkSessionHandlerRegistry Handlers => throw new NotSupportedException();

        public ValueTask CloseAsync() => ValueTask.CompletedTask;
    }

    private sealed class AuthenticatePacketHandler : IZLinkSessionPacketHandler<SessionPacketContext, AuthenticateReply>
    {
        public ValueTask HandleAsync(
            SessionPacketContext context,
            ZLinkSessionDispatchContext dispatch,
            AuthenticateReply message,
            CancellationToken cancellationToken)
        {
            _ = message;
            cancellationToken.ThrowIfCancellationRequested();
            context.LastPacketName = dispatch.PacketName;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class ExampleSessionHandlerRegistry(SessionPacketContext context) : IZLinkSessionHandlerRegistry
    {
        private Func<ZLinkSessionDispatchContext, ZLinkMessage, CancellationToken, ValueTask>? _handler;

        public void AddHandler<THandler>()
            where THandler : class
        {
            _handler = async (dispatch, payload, cancellationToken) =>
            {
                var handler = new AuthenticatePacketHandler();
                await ((IZLinkSessionPacketHandler<SessionPacketContext, AuthenticateReply>)handler)
                    .HandleAsync(context, dispatch, payload.Decode<AuthenticateReply>(), cancellationToken);
            };
        }

        public void AddHandler<THandler>(string packetName)
            where THandler : class
        {
            _ = packetName;
            AddHandler<THandler>();
        }

        public async ValueTask<bool> TryHandleAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken = default)
        {
            if (dispatch.PacketName != nameof(AuthenticateReply) || _handler is null) return false;

            await _handler(dispatch, payload, cancellationToken);
            return true;
        }
    }

    private sealed class ExampleSession(IZLinkSessionContext context) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }
    }

    private sealed class ExampleSessionContext :
        IZLinkSessionContext,
        IZLinkSessionClient,
        IZLinkSessionActors,
        IZLinkStream
    {
        private readonly Dictionary<string, IZLinkSessionActor> _actors = new(StringComparer.Ordinal);

        public bool IsClosed { get; private set; }

        public bool StreamClosed { get; private set; }

        public IReadOnlyCollection<IZLinkSessionActor> Bound => _actors.Values.ToArray();

        public ValueTask<IZLinkSessionActor> BindAsync(
            Systems.Zlink.ActorRef actorRef,
            CancellationToken cancellationToken = default)
        {
            var actor = new ActorRef(actorRef);
            _actors[actorRef.ActorId] = actor;
            return ValueTask.FromResult<IZLinkSessionActor>(actor);
        }

        public async ValueTask<IZLinkSessionActor> BindOrGetAsync(
            Systems.Zlink.ActorRef actorRef,
            CancellationToken cancellationToken = default)
        {
            return Find(actorRef.ActorId)
                   ?? await BindAsync(actorRef, cancellationToken);
        }

        public IZLinkSessionActor? Find(string actorId)
        {
            return _actors.GetValueOrDefault(actorId);
        }

        public IZLinkSessionSendCall Send<TMessage>(TMessage message)
        {
            return new SessionSendCall();
        }

        public IZLinkSessionReplyCall Reply<TMessage>(TMessage message)
        {
            return new SessionReplyCall();
        }

        public string SessionId => "session-1";

        public RoutingId? RoutingId => Systems.Zlink.RoutingId.From("session-route");

        public string? LocalAddr => "tcp://127.0.0.1:5000";

        public string? RemoteAddr => "tcp://127.0.0.1:5001";

        public IZLinkSessionClient Client => this;

        public IZLinkSessionActors Actors => this;

        public IZLinkSessionHandlerRegistry Handlers { get; } =
            new ExampleSessionHandlerRegistry(new SessionPacketContext());

        public ValueTask CloseAsync()
        {
            IsClosed = true;
            return ValueTask.CompletedTask;
        }

        bool IZLinkStream.Write(
            ZLinkMessage payload,
            SendFlags flags)
        {
            return true;
        }

        async ValueTask IZLinkStream.CloseAsync()
        {
            StreamClosed = true;
            await CloseAsync();
        }
    }

    private sealed class ExampleBoundSession : IZLinkBoundSession
    {
        public bool IsDisconnected { get; private set; }

        public IZLinkBoundSessionSendCall Send<TMessage>(TMessage message)
        {
            return new BoundSessionSendCall();
        }

        public ValueTask DisconnectAsync(CancellationToken cancellationToken = default)
        {
            IsDisconnected = true;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class ActorRef(Systems.Zlink.ActorRef actor) : IZLinkSessionActor
    {
        public string ActorId => Ref.ActorId;

        public Systems.Zlink.ActorRef Ref { get; } = actor;

        public ValueTask RelayAsync(
            ZLinkMessage payload,
            CancellationToken cancellationToken = default)
        {
            return ValueTask.CompletedTask;
        }

        public ValueTask NotifyDisconnectedAsync(CancellationToken cancellationToken = default)
        {
            return ValueTask.CompletedTask;
        }
    }

    private sealed class SendCall : IZLinkSendCall
    {
        public IZLinkSendCall Metadata(string key, string value) => this;

        public IZLinkSendCall Metadata(ZLinkMessageMetadata metadata) => this;

        public ValueTask Async(CancellationToken cancellationToken = default) =>
            ValueTask.CompletedTask;
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

    private sealed class SessionSendCall : IZLinkSessionSendCall
    {
        public IZLinkSessionSendCall Metadata(string key, string value)
        {
            return this;
        }

        public IZLinkSessionSendCall Metadata(ZLinkMessageMetadata metadata)
        {
            return this;
        }

        public IZLinkSessionSendCall Compress()
        {
            return this;
        }

        public ValueTask Async(
            CancellationToken cancellationToken = default) =>
            ValueTask.CompletedTask;
    }

    private sealed class SessionReplyCall : IZLinkSessionReplyCall
    {
        public IZLinkSessionReplyCall Compress()
        {
            return this;
        }

        public ValueTask Async(
            CancellationToken cancellationToken = default) =>
            ValueTask.CompletedTask;
    }

    private sealed class BoundSessionSendCall : IZLinkBoundSessionSendCall
    {
        public IZLinkBoundSessionSendCall Metadata(string key, string value)
        {
            return this;
        }

        public IZLinkBoundSessionSendCall Metadata(ZLinkMessageMetadata metadata)
        {
            return this;
        }

        public ValueTask Async(
            CancellationToken cancellationToken = default) =>
            ValueTask.CompletedTask;
    }

    private sealed class MetadataPolicy(IReadOnlySet<string> forwardedKeys) : IZLinkMessageMetadataPolicy
    {
        public bool CanForward(string key)
        {
            return forwardedKeys.Contains(key);
        }
    }
}
