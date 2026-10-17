using System.Text.Json;
using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;

namespace MyGame.Zone;

public sealed class PlayerActor(IZLinkActorContext context, NodeSettings settings) : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;
    public int X { get; private set; }
    public int Sequence { get; private set; }
    public string ZoneId => GameNames.ZoneId(X);

    public void Apply(EnterZone position)
    {
        X = position.X;
        Sequence = position.Sequence;
    }

    public void Move(EnterZone destination)
    {
        Context.JoinSpot(GameNames.ZoneId(destination.X), destination).Defer();
    }

    public WorldState State() =>
        new(Context.ActorId, Context.ObjectGeneration, ZoneId, settings.Id, X, Sequence);

    public ValueTask SendStateAsync() => Context.BoundSession.Send(State()).Async();

    public async ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (completion is not ZLinkActorJoinCompletion.Accepted)
            throw new InvalidOperationException($"Zone join failed: {completion}.");
        await SendStateAsync();
    }

    public PlayerSnapshot Capture() => new(X, Sequence);

    public void Restore(PlayerSnapshot state)
    {
        Apply(new EnterZone(state.X, state.Sequence));
    }
}

public sealed record PlayerSnapshot(int X, int Sequence);

public sealed class PlayerFactory(NodeSettings settings) : IZLinkActorFactory<PlayerActor>
{
    public ValueTask<PlayerActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new PlayerActor(context, settings));
    }
}

// Only application state is serialized. Framework owns routes, membership, queues and timers.
public sealed class PlayerRelocation : IZLinkActorRelocationAdapter<PlayerActor>
{
    public ValueTask<byte[]> CaptureAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(JsonSerializer.SerializeToUtf8Bytes(actor.Capture()));
    }

    public ValueTask RestoreAsync(
        PlayerActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        actor.Restore(
            JsonSerializer.Deserialize<PlayerSnapshot>(payload.Span)
                ?? throw new InvalidDataException("Missing player relocation state.")
        );
        return ValueTask.CompletedTask;
    }
}
