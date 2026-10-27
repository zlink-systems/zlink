using Zlink.Framework.Contracts.Actors;

namespace MyGame.Shared;

public sealed class PlayerActor(IZLinkActorContext context, IZLinkActorClient actors) : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;

    public async ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken ct
    )
    {
        ct.ThrowIfCancellationRequested();
        if (completion is ZLinkActorJoinCompletion.Rejected rejected)
        {
            var reply =
                rejected.Reply?.Decode<RegionFull>()
                ?? throw new InvalidDataException(
                    "Region rejection omitted its application reply."
                );
            // The current Entry handles target selection after the rejected join.
            await actors.SendToActor(Context.ActorId, reply).Async();
        }
        else if (completion is ZLinkActorJoinCompletion.Failed failed)
            throw new InvalidOperationException($"Region join failed: {failed.Kind}.");
    }
}

public sealed class PlayerFactory(IZLinkActorClient actors) : IZLinkActorFactory<PlayerActor>
{
    public ValueTask<PlayerActor> CreateAsync(IZLinkActorContext context, CancellationToken ct) =>
        ValueTask.FromResult(new PlayerActor(context, actors));
}

// Identity, membership and binding are Framework-owned, with no application Actor state.
public sealed class PlayerRelocation : IZLinkActorRelocationAdapter<PlayerActor>
{
    public ValueTask<byte[]> CaptureAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.FromResult(Array.Empty<byte>());

    public ValueTask RestoreAsync(
        PlayerActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken ct
    ) => ValueTask.CompletedTask;
}
