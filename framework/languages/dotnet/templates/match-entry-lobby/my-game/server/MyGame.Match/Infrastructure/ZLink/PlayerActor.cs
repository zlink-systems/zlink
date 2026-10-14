using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;

namespace MyGame.Match;

public sealed class PlayerActor(IZLinkActorContext context) : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;

    public async ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken
    )
    {
        if (completion is ZLinkActorJoinCompletion.Rejected)
            await Context.BoundSession.Send(new ActionRejected(Rejection.AdmissionRejected)).Async();
        else if (completion is ZLinkActorJoinCompletion.Failed failed)
            throw new InvalidOperationException($"Spot join failed: {failed.Kind}.");
    }
}

public sealed class PlayerFactory : IZLinkActorFactory<PlayerActor>
{
    public ValueTask<PlayerActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken
    ) => ValueTask.FromResult(new PlayerActor(context));
}

// Player identity and binding are Framework-owned; there is no application state to transfer.
public sealed class PlayerRelocation : IZLinkActorRelocationAdapter<PlayerActor>
{
    public ValueTask<byte[]> CaptureAsync(PlayerActor actor, CancellationToken cancellationToken) =>
        ValueTask.FromResult(Array.Empty<byte>());

    public ValueTask RestoreAsync(
        PlayerActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken
    ) => ValueTask.CompletedTask;
}
