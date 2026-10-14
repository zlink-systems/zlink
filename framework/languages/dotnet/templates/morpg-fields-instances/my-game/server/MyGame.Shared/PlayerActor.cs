using System.Text.Json;
using Zlink.Framework.Contracts.Actors;

namespace MyGame.Shared;

public sealed class PlayerActor(IZLinkActorContext context, NodeSettings settings) : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;
    public int X { get; private set; }
    public int Sequence { get; private set; }
    public string FieldId => GameNames.FieldId(X);

    public void Apply(EnterField position)
    {
        X = position.X;
        Sequence = position.Sequence;
    }

    public void Move(EnterField destination)
    {
        Context.JoinSpot(GameNames.FieldId(destination.X), destination).Defer();
    }

    public ValueTask SendStateAsync() =>
        Context.BoundSession.Send(new WorldState(FieldId, settings.Id, X, Sequence)).Async();

    public async ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (completion is not ZLinkActorJoinCompletion.Accepted)
            throw new InvalidOperationException($"Spot join failed: {completion}.");
        if (Context.SpotId == FieldId) await SendStateAsync();
    }

    public PlayerSnapshot Capture() => new(X, Sequence);

    public void Restore(PlayerSnapshot state)
    {
        Apply(new EnterField(state.X, state.Sequence));
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
