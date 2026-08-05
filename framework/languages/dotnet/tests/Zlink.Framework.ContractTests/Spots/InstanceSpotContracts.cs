using System.Text;
using System.Text.Json;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Workers;
using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Spots;

/// <summary>
///     Worked examples for the actor-free Spot kind and for the two manager
///     calls that create a User Spot. Per 05-spots §8 an Instance Spot is
///     activated by the first explicitly marked message rather than by the
///     manager, so its lifecycle has no <c>OnCreateAsync</c> and its context
///     has no Actor membership. The manager owns User Spot
///     <c>Create</c>/<c>GetOrCreate</c> only.
/// </summary>
public sealed class InstanceSpotContracts
{
    [Fact]
    [ContractExample(
        typeof(IZLinkInstanceSpot),
        typeof(IZLinkInstanceSpotContext),
        typeof(IZLinkInstanceSpotHandlerRegistry))]
    public async Task Instance_spot_registers_direct_packets_and_timers_but_never_actor_membership()
    {
        // A per-region leaderboard: one live instance per region, addressed
        // by a stable SpotId, activated cold on the first message.
        var context = new ExampleInstanceSpotContext("leaderboard-eu-west");
        IZLinkInstanceSpot spot = new LeaderboardSpot(context);

        spot.Configure();
        await spot.OnInitializeAsync(CancellationToken.None);

        Assert.Equal(["leaderboard.score-posted"], context.Handlers.Packets);
        Assert.Equal(["flush"], context.Timers);
        Assert.Equal("leaderboard-eu-west", context.SpotId);
        Assert.Equal("play", context.MeshName);

        // Outbound is the same Spot outbound every Spot kind gets, so an
        // Instance Spot can answer a channel and address another instance.
        await context.Outbound
            .SendToChannel("analytics", new ScorePosted("player-8821", 1901))
            .Async();
        await context.Outbound
            .SendToSpot(
                "leaderboard-us-east",
                new ScorePosted("player-8821", 1901))
            .InstanceSpot("leaderboard")
            .InMesh("play")
            .Async();

        // Closing carries why and by when, and the instance closes itself
        // through the context rather than through a manager.
        Assert.True(await context.CloseAsync());
        await spot.OnClosingAsync(
            new ZLinkSpotClosingContext(
                ZLinkSpotCloseReason.ExplicitClose,
                DateTimeOffset.UtcNow.AddSeconds(3)),
            CancellationToken.None);
        Assert.True(context.Closed);

        // Actor-free by contract: no membership callbacks on the lifecycle and
        // no membership operations on the context.
        Assert.False(typeof(IZLinkInstanceSpot).IsAssignableTo(typeof(IZLinkSpot)));
        Assert.Null(typeof(IZLinkInstanceSpot).GetMethod("OnCreateAsync"));
        Assert.Null(typeof(IZLinkInstanceSpotContext).GetMethod("LeaveActorAsync"));
        Assert.Null(typeof(IZLinkInstanceSpotContext).GetMethod("DestroyActorAsync"));

        // Its registry accepts direct packets only; actor packets and
        // Logical Multicast subscriptions are rejected at registration.
        Assert.Equal(
            new[] { "AddPacket" },
            typeof(IZLinkInstanceSpotHandlerRegistry)
                .GetMethods()
                .Select(method => method.Name)
                .ToArray());
    }

    [Fact]
    [ContractExample(
        typeof(IZLinkSpotRelocationAdapter<>),
        typeof(IZLinkInstanceSpotFactoryBuilder<>))]
    public async Task Spot_relocation_adapter_round_trips_only_application_state()
    {
        // PreserveStateWith<TAdapter>() selects the adapter that carries
        // application state to the node that takes the instance over.
        var preserve = typeof(IZLinkInstanceSpotFactoryBuilder<LeaderboardSpot>)
            .GetMethods()
            .Single(method => method.Name == "PreserveStateWith");
        Assert.True(preserve.IsGenericMethodDefinition);

        var adapter = new LeaderboardRelocationAdapter();
        var source = new LeaderboardSpot(new ExampleInstanceSpotContext("leaderboard-eu-west"));
        source.Post("player-8821", 1901);
        source.Post("player-4013", 1744);

        var captured = await adapter.CaptureAsync(source, CancellationToken.None);

        var target = new LeaderboardSpot(new ExampleInstanceSpotContext("leaderboard-eu-west"));
        Assert.Empty(target.Scores);
        await adapter.RestoreAsync(target, captured, CancellationToken.None);

        Assert.Equal(source.Scores, target.Scores);
        Assert.Equal(1901, target.Scores["player-8821"]);

        var methods = typeof(IZLinkSpotRelocationAdapter<LeaderboardSpot>).GetMethods();
        Assert.Equal(
            typeof(ValueTask<byte[]>),
            methods.Single(method => method.Name == "CaptureAsync").ReturnType);
        Assert.Equal(
            [typeof(LeaderboardSpot), typeof(ReadOnlyMemory<byte>), typeof(CancellationToken)],
            methods
                .Single(method => method.Name == "RestoreAsync")
                .GetParameters()
                .Select(parameter => parameter.ParameterType));
    }

    [Fact]
    [ContractExample(
        typeof(IZLinkSpotCreateCall),
        typeof(IZLinkSpotGetOrCreateCall),
        typeof(IZLinkSpotManager))]
    public async Task Spot_manager_create_issues_an_id_and_get_or_create_returns_the_existing_one()
    {
        var manager = new ExampleSpotManager();

        // Create: the framework issues the global SpotId. The caller supplies
        // the stable type, the mesh to place it in, the creation request and
        // one deadline covering reservation, factory and reply.
        var created = await manager
            .Create("battle-room")
            .InMesh("play")
            .Request(new OpenBattle("ranked-3v3", 6))
            .Timeout(TimeSpan.FromSeconds(5))
            .Async();
        Assert.Equal(ZLinkSpotCreateState.Created, created.State);
        Assert.Equal("play", created.Spot.MeshName);
        Assert.Equal(1UL, created.Spot.ObjectGeneration);
        Assert.StartsWith("battle-room-", created.Spot.SpotId, StringComparison.Ordinal);

        // GetOrCreate: the caller owns the SpotId, so a lobby that reconnects
        // to the same guild hall gets Existing rather than a second Spot. The
        // CAS loser never runs a second factory.
        var factoryRunsBeforeGetOrCreate = manager.FactoryRuns;
        var first = await manager
            .GetOrCreate("guild-hall-4471", "guild-hall")
            .InMesh("play")
            .Request(ZLinkMessage.From(new OpenBattle("social", 40)))
            .Timeout(TimeSpan.FromSeconds(5))
            .Async();
        var second = await manager
            .GetOrCreate("guild-hall-4471", "guild-hall")
            .Async();

        Assert.Equal(ZLinkSpotCreateState.Created, first.State);
        Assert.Equal(ZLinkSpotCreateState.Existing, second.State);
        Assert.Equal(first.Spot, second.Spot);
        Assert.Equal(factoryRunsBeforeGetOrCreate + 1, manager.FactoryRuns);

        // A different stable type on the same SpotId is a type mismatch, not
        // a silent replacement.
        Assert.Equal(
            ZLinkSpotCreateState.Rejected,
            (await manager.GetOrCreate("guild-hall-4471", "battle-room").Async()).State);

        Assert.Equal(second.Spot, await manager.FindAsync("guild-hall-4471"));
        Assert.True(await manager.CloseAsync(second.Spot));
        Assert.Null(await manager.FindAsync("guild-hall-4471"));

        // Exact-incarnation close: a stale ref closes nothing.
        Assert.False(await manager.CloseAsync(first.Spot));

        // The manager has no Instance Spot overload; that path is the
        // explicit InstanceSpot marker on a Spot message call.
        Assert.Equal(
            new[] { "CloseAsync", "Create", "FindAsync", "GetOrCreate" },
            typeof(IZLinkSpotManager).GetMethods().Select(method => method.Name).Order().ToArray());
    }

    private sealed record ScorePosted(string ActorId, int Rating);

    private sealed record OpenBattle(string Mode, int Capacity);

    private sealed class LeaderboardSpot(IZLinkInstanceSpotContext context) : IZLinkInstanceSpot
    {
        public IZLinkInstanceSpotContext Context { get; } = context;

        public Dictionary<string, int> Scores { get; } = new(StringComparer.Ordinal);

        public void Post(string actorId, int rating) => Scores[actorId] = rating;

        public void Configure() => Context.Handlers.AddPacket<ScorePostedHandler>();

        public async ValueTask OnInitializeAsync(CancellationToken cancellationToken)
        {
            await Context.AddTimer<FlushHandler>(
                "flush",
                TimeSpan.FromSeconds(30),
                cancellationToken: cancellationToken);
        }
    }

    private sealed class ScorePostedHandler : IZLinkSpotPacketHandler<LeaderboardSpot, ScorePosted>
    {
        public ValueTask HandleAsync(
            LeaderboardSpot spot,
            ScorePosted message,
            CancellationToken cancellationToken)
        {
            spot.Post(message.ActorId, message.Rating);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class FlushHandler : IZLinkSpotTimerHandler<LeaderboardSpot>
    {
        public ValueTask HandleAsync(
            LeaderboardSpot spot,
            ZLinkTimerTick tick,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }

    private sealed class LeaderboardRelocationAdapter : IZLinkSpotRelocationAdapter<LeaderboardSpot>
    {
        public ValueTask<byte[]> CaptureAsync(
            LeaderboardSpot spot,
            CancellationToken cancellationToken) =>
            ValueTask.FromResult(
                Encoding.UTF8.GetBytes(JsonSerializer.Serialize(spot.Scores)));

        public ValueTask RestoreAsync(
            LeaderboardSpot spot,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken)
        {
            var scores = JsonSerializer.Deserialize<Dictionary<string, int>>(payload.Span)!;
            foreach (var (actorId, rating) in scores)
                spot.Post(actorId, rating);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class ExampleSpotManager : IZLinkSpotManager
    {
        private readonly Dictionary<string, (SpotRef Spot, string SpotType)> _spots =
            new(StringComparer.Ordinal);

        private int _issuedIds;

        public int FactoryRuns { get; private set; }

        public IZLinkSpotCreateCall Create(string spotType) =>
            new SpotCreateCall(this, spotType);

        public IZLinkSpotGetOrCreateCall GetOrCreate(string spotId, string spotType) =>
            new SpotGetOrCreateCall(this, spotId, spotType);

        public ValueTask<SpotRef?> FindAsync(
            string spotId,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult<SpotRef?>(
                _spots.TryGetValue(spotId, out var entry) ? entry.Spot : null);

        public ValueTask<bool> CloseAsync(
            SpotRef spot,
            CancellationToken cancellationToken = default)
        {
            if (!_spots.TryGetValue(spot.SpotId, out var entry) || entry.Spot != spot)
                return ValueTask.FromResult(false);

            _spots.Remove(spot.SpotId);
            return ValueTask.FromResult(true);
        }

        private ZLinkSpotCreateResult Submit(string? spotId, string spotType, string meshName)
        {
            if (spotId is not null && _spots.TryGetValue(spotId, out var existing))
            {
                return string.Equals(existing.SpotType, spotType, StringComparison.Ordinal)
                    ? new ZLinkSpotCreateResult(
                        existing.Spot,
                        ZLinkSpotCreateState.Existing,
                        null)
                    : new ZLinkSpotCreateResult(
                        default,
                        ZLinkSpotCreateState.Rejected,
                        ZLinkMessage.From("SpotTypeMismatch"));
            }

            FactoryRuns++;
            var id = spotId ?? $"{spotType}-{++_issuedIds:D4}";
            var spot = new SpotRef(id, 1, meshName, RoutingId.From("play-node-b"));
            _spots[id] = (spot, spotType);
            return new ZLinkSpotCreateResult(spot, ZLinkSpotCreateState.Created, null);
        }

        private sealed class SpotCreateCall(ExampleSpotManager manager, string spotType)
            : IZLinkSpotCreateCall
        {
            private string _meshName = "play";

            public IZLinkSpotCreateCall InMesh(string meshName)
            {
                _meshName = meshName;
                return this;
            }

            public IZLinkSpotCreateCall Request(ZLinkMessage request) => this;

            public IZLinkSpotCreateCall Request<TRequest>(TRequest request) =>
                Request(ZLinkMessage.From(request));

            public IZLinkSpotCreateCall Timeout(TimeSpan timeout) => this;

            public ValueTask<ZLinkSpotCreateResult> Async(
                CancellationToken cancellationToken = default) =>
                ValueTask.FromResult(manager.Submit(spotId: null, spotType, _meshName));

            public ValueTask<ZLinkSpotCreateResult> Yield(
                CancellationToken cancellationToken = default) =>
                ValueTask.FromResult(manager.Submit(spotId: null, spotType, _meshName));
        }

        private sealed class SpotGetOrCreateCall(
            ExampleSpotManager manager,
            string spotId,
            string spotType) : IZLinkSpotGetOrCreateCall
        {
            private string _meshName = "play";

            public IZLinkSpotGetOrCreateCall InMesh(string meshName)
            {
                _meshName = meshName;
                return this;
            }

            public IZLinkSpotGetOrCreateCall Request(ZLinkMessage request) => this;

            public IZLinkSpotGetOrCreateCall Request<TRequest>(TRequest request) =>
                Request(ZLinkMessage.From(request));

            public IZLinkSpotGetOrCreateCall Timeout(TimeSpan timeout) => this;

            public ValueTask<ZLinkSpotCreateResult> Async(
                CancellationToken cancellationToken = default) =>
                ValueTask.FromResult(manager.Submit(spotId, spotType, _meshName));

            public ValueTask<ZLinkSpotCreateResult> Yield(
                CancellationToken cancellationToken = default) =>
                ValueTask.FromResult(manager.Submit(spotId, spotType, _meshName));
        }
    }

    private sealed class ExampleInstanceSpotContext(string spotId) : IZLinkInstanceSpotContext
    {
        public string MeshName => "play";

        public string SpotId { get; } = spotId;

        public ulong ObjectGeneration => 1;

        public RoutingId NodeRid => RoutingId.From("play-node-b");

        public bool Closed { get; private set; }

        public List<string> Timers { get; } = [];

        public ExampleInstanceSpotHandlerRegistry Handlers { get; } = new();

        IZLinkInstanceSpotHandlerRegistry IZLinkInstanceSpotContext.Handlers => Handlers;

        public IZLinkSpotOutbound Outbound { get; } = new ExampleSpotOutbound();

        public ValueTask<bool> CloseAsync(CancellationToken cancellationToken = default)
        {
            Closed = true;
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
            return ValueTask.FromResult<IZLinkTimer>(new ExampleTimer());
        }

        public IZLinkWorkerCall<TResult> RunCpuWorker<TResult>(
            Func<CancellationToken, TResult> work) => new ExampleWorkerCall<TResult>(work);

        public IZLinkWorkerCall<TResult> RunIoWorker<TResult>(
            Func<CancellationToken, ValueTask<TResult>> work) =>
            new ExampleWorkerCall<TResult>(token => work(token).AsTask().GetAwaiter().GetResult());
    }

    private sealed class ExampleInstanceSpotHandlerRegistry : IZLinkInstanceSpotHandlerRegistry
    {
        public List<string> Packets { get; } = [];

        public void AddPacket<THandler>()
            where THandler : class
        {
            // The registered packet name is the handler's message contract;
            // the leaderboard registers exactly one direct packet.
            Packets.Add("leaderboard.score-posted");
        }
    }

    private sealed class ExampleSpotOutbound : IZLinkSpotOutbound
    {
        public IZLinkSpotSendCall SendToSpot<TMessage>(string spotId, TMessage message) =>
            new ExampleSpotSendCall();

        public IZLinkSpotRequestCall RequestToSpot<TRequest>(string spotId, TRequest request) =>
            new ExampleSpotRequestCall();

        public IZLinkPublishCall Publish<TEvent>(string channelName, string topic, TEvent message) =>
            new ExamplePublishCall();

        public IZLinkSendCall SendToChannel<TMessage>(string channelName, TMessage message) =>
            new ExampleSendCall();

        public IZLinkRequestCall RequestToChannel<TRequest>(string channelName, TRequest request) =>
            new ExampleRequestCall();
    }

    private sealed class ExampleSpotSendCall : IZLinkSpotSendCall
    {
        public IZLinkSpotSendCall InstanceSpot() => this;
        public IZLinkSpotSendCall InstanceSpot(string instanceSpotType) => this;
        public IZLinkSpotSendCall InMesh(string meshName) => this;
        public IZLinkSpotSendCall Metadata(string key, string value) => this;
        public IZLinkSpotSendCall Metadata(ZLinkMessageMetadata metadata) => this;
        public ValueTask Async(CancellationToken cancellationToken = default) =>
            ValueTask.CompletedTask;
    }

    private sealed class ExampleSpotRequestCall : IZLinkSpotRequestCall
    {
        public IZLinkSpotRequestCall InstanceSpot() => this;
        public IZLinkSpotRequestCall InstanceSpot(string instanceSpotType) => this;
        public IZLinkSpotRequestCall InMesh(string meshName) => this;
        public IZLinkSpotRequestCall Metadata(string key, string value) => this;
        public IZLinkSpotRequestCall Metadata(ZLinkMessageMetadata metadata) => this;
        public IZLinkSpotRequestCall Timeout(TimeSpan timeout) => this;
        public ValueTask<TReply> Async<TReply>(CancellationToken cancellationToken = default) =>
            ValueTask.FromResult<TReply>(default!);
        public ValueTask<TReply> Yield<TReply>(CancellationToken cancellationToken = default) =>
            ValueTask.FromResult<TReply>(default!);
    }

    private sealed class ExampleSendCall : IZLinkSendCall
    {
        public IZLinkSendCall Metadata(string key, string value) => this;

        public IZLinkSendCall Metadata(ZLinkMessageMetadata metadata) => this;

        public ValueTask Async(CancellationToken cancellationToken = default) =>
            ValueTask.CompletedTask;
    }

    private sealed class ExampleRequestCall : IZLinkRequestCall
    {
        public IZLinkRequestCall Metadata(string key, string value) => this;

        public IZLinkRequestCall Metadata(ZLinkMessageMetadata metadata) => this;

        public IZLinkRequestCall Timeout(TimeSpan timeout) => this;

        public ValueTask<TReply> Async<TReply>(CancellationToken cancellationToken = default) =>
            ValueTask.FromResult<TReply>(default!);

        public ValueTask<TReply> Yield<TReply>(CancellationToken cancellationToken = default) =>
            ValueTask.FromResult<TReply>(default!);
    }

    private sealed class ExamplePublishCall : IZLinkPublishCall
    {
        public IZLinkPublishCall Metadata(string key, string value) => this;

        public IZLinkPublishCall Metadata(ZLinkMessageMetadata metadata) => this;

        public ValueTask Async(CancellationToken cancellationToken = default) =>
            ValueTask.CompletedTask;
    }

    private sealed class ExampleTimer : IZLinkTimer
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

    private sealed class ExampleWorkerCall<TResult>(Func<CancellationToken, TResult> work)
        : IZLinkWorkerCall<TResult>
    {
        public void Submit(CancellationToken cancellationToken = default) => _ = work(cancellationToken);

        public IZLinkWorkerCall<TResult> Timeout(TimeSpan timeout) => this;

        public ValueTask<TResult> Async(CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(work(cancellationToken));

        public ValueTask<TResult> Yield(CancellationToken cancellationToken = default) =>
            Async(cancellationToken);
    }
}
