using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;

namespace MyGame.Zone;

public sealed class PlayerActor(IZLinkActorContext context) : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;

    public void Move(string zoneId, EnterZone destination) =>
        Context.JoinSpot(zoneId, destination).Defer();

    public async ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (completion is not ZLinkActorJoinCompletion.Accepted accepted)
            throw new InvalidOperationException($"Zone join failed: {completion}.");
        var reply = accepted.Reply
            ?? throw new InvalidDataException("Accepted Zone join has no WorldState reply.");
        await Context.BoundSession.Send(reply.Decode<WorldState>()).Async();
    }
}

public sealed class PlayerFactory : IZLinkActorFactory<PlayerActor>
{
    public ValueTask<PlayerActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new PlayerActor(context));
    }
}

public sealed class PlayerRelocation : IZLinkActorRelocationAdapter<PlayerActor>
{
    public ValueTask<byte[]> CaptureAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(Array.Empty<byte>());
    }

    public ValueTask RestoreAsync(
        PlayerActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!payload.IsEmpty)
            throw new InvalidDataException("Player Actor has no application relocation state.");
        return ValueTask.CompletedTask;
    }
}
