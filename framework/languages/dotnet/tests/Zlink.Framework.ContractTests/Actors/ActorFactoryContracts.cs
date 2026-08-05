using System.Text;
using System.Text.Json;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Actors;

/// <summary>
///     Worked examples for the two contracts an actor type registers with its
///     factory: the typed factory the runtime calls to materialize an actor
///     (06-actors §2) and the relocation adapter selected with
///     <c>PreserveStateWith</c> to move that actor's application state across
///     nodes (06-actors §5).
/// </summary>
public sealed class ActorFactoryContracts
{
    [Fact]
    [ContractExample(typeof(IZLinkActorFactory<>), typeof(IZLinkActorFactory))]
    public async Task Typed_actor_factory_is_the_only_place_an_actor_type_is_constructed()
    {
        // Applications register the typed factory
        // (AddActorFactory<PlayerActor, PlayerActorFactory>). The framework's
        // registry keeps actors of every type in one collection, so it calls
        // the non-generic bridge that the typed contract already implements.
        var factory = new PlayerActorFactory(startingLoadout: "torch,potion");
        var context = new ExampleActorContext("player-8821");

        var typed = await factory.CreateAsync(context);
        Assert.Equal("player-8821", typed.Context.ActorId);
        Assert.Same(context, typed.Context);
        Assert.Equal("torch,potion", typed.Loadout);

        IZLinkActorFactory untyped = factory;
        var bridgedContext = new ExampleActorContext("player-8822");
        var bridged = await untyped.CreateAsync(bridgedContext);

        // The bridge is a default implementation on the typed contract, so
        // both entry points build the same concrete actor with the same
        // constructor dependencies.
        var bridgedPlayer = Assert.IsType<PlayerActor>(bridged);
        Assert.Equal("player-8822", bridgedPlayer.Context.ActorId);
        Assert.Equal("torch,potion", bridgedPlayer.Loadout);
        Assert.Equal(2, factory.CreatedCount);

        // Context owns identity; the factory does not receive a duplicate ID.
        var typedCreate = typeof(IZLinkActorFactory<PlayerActor>)
            .GetMethod(nameof(IZLinkActorFactory<PlayerActor>.CreateAsync))!;
        Assert.Equal(
            [typeof(IZLinkActorContext), typeof(CancellationToken)],
            typedCreate.GetParameters().Select(parameter => parameter.ParameterType));
        Assert.Equal(typeof(ValueTask<PlayerActor>), typedCreate.ReturnType);
        Assert.Null(typeof(IZLinkActor).GetProperty("ActorId"));
    }

    [Fact]
    [ContractExample(
        typeof(IZLinkActorRelocationAdapter<>),
        typeof(IZLinkActorFactory<>),
        typeof(IZLinkActorFactoryBuilder<>))]
    public async Task Preserve_state_policy_moves_application_state_through_the_relocation_adapter()
    {
        // Registration decides how a cross-node move materializes the actor.
        // The builder requires exactly one of DisableRelocation,
        // RecreateOnRelocation or PreserveStateWith. The last choice names
        // the adapter that round-trips application state.
        var builderMethods = typeof(IZLinkActorFactoryBuilder<PlayerActor>)
            .GetMethods();
        Assert.Contains(
            builderMethods,
            method => method.Name == "DisableRelocation");
        Assert.Contains(
            builderMethods,
            method => method.Name == "RecreateOnRelocation");
        Assert.Contains(
            builderMethods,
            method => method.Name == "PreserveStateWith"
                      && method.IsGenericMethodDefinition);

        var factory = new PlayerActorFactory(startingLoadout: "torch,potion");
        var adapter = new PlayerRelocationAdapter();

        // Source node: the actor has played for a while before play-node-b
        // starts retiring.
        var source = await factory.CreateAsync(
            new ExampleActorContext("player-8821"));
        source.Rating = 1901;
        source.Loadout = "torch,potion,greatsword";

        var captured = await adapter.CaptureAsync(source, CancellationToken.None);

        // Target node: Recreate's factory call happens first, so the restored
        // actor starts from the registered defaults and the adapter is the
        // only thing that carries the played-out state over.
        var target = await factory.CreateAsync(
            new ExampleActorContext("player-8821"));
        Assert.Equal(0, target.Rating);
        Assert.Equal("torch,potion", target.Loadout);

        await adapter.RestoreAsync(target, captured, CancellationToken.None);

        Assert.Equal(source.Rating, target.Rating);
        Assert.Equal(source.Loadout, target.Loadout);
        Assert.Equal(source.Context.ActorId, target.Context.ActorId);

        // Capture returns an owned byte[] and restore takes a read-only view;
        // the adapter sees application state only, never the relocation
        // reference, journal, phase or store CAS version.
        var adapterMethods = typeof(IZLinkActorRelocationAdapter<PlayerActor>).GetMethods();
        Assert.Equal(
            typeof(ValueTask<byte[]>),
            adapterMethods.Single(method => method.Name == "CaptureAsync").ReturnType);
        Assert.Equal(
            [typeof(PlayerActor), typeof(ReadOnlyMemory<byte>), typeof(CancellationToken)],
            adapterMethods
                .Single(method => method.Name == "RestoreAsync")
                .GetParameters()
                .Select(parameter => parameter.ParameterType));
    }

    private sealed class PlayerActor(IZLinkActorContext context, string loadout)
        : IZLinkActor
    {
        public IZLinkActorContext Context { get; } = context;

        public int Rating { get; set; }

        public string Loadout { get; set; } = loadout;
    }

    private sealed class PlayerActorFactory(string startingLoadout) : IZLinkActorFactory<PlayerActor>
    {
        public int CreatedCount { get; private set; }

        public ValueTask<PlayerActor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken cancellationToken = default)
        {
            CreatedCount++;
            return ValueTask.FromResult(new PlayerActor(context, startingLoadout));
        }
    }

    private sealed class PlayerRelocationAdapter : IZLinkActorRelocationAdapter<PlayerActor>
    {
        public ValueTask<byte[]> CaptureAsync(
            PlayerActor actor,
            CancellationToken cancellationToken)
        {
            var state = JsonSerializer.Serialize(
                new PlayerState(actor.Rating, actor.Loadout));
            return ValueTask.FromResult(Encoding.UTF8.GetBytes(state));
        }

        public ValueTask RestoreAsync(
            PlayerActor actor,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken)
        {
            var state = JsonSerializer.Deserialize<PlayerState>(payload.Span)!;
            actor.Rating = state.Rating;
            actor.Loadout = state.Loadout;
            return ValueTask.CompletedTask;
        }

        private sealed record PlayerState(int Rating, string Loadout);
    }

    private sealed class ExampleActorContext(string actorId) : IZLinkActorContext
    {
        public string ActorId => actorId;

        public ulong ObjectGeneration => 3;

        public string MeshName => "play";

        public string? SpotId => "battle-1042";

        public IZLinkBoundSession BoundSession => null!;

        public IZLinkActorJoinSpotCall JoinSpot(string spotId, ZLinkMessage request) => null!;

        public IZLinkActorJoinEntrySpotCall JoinEntrySpot(ZLinkMessage request) => null!;
    }
}
