using System.Text.Json;
using MyGame.Channel.Domain;
using MyGame.Channel.Infrastructure.Redis;
using Zlink.Framework.Contracts.Actors;

namespace MyGame.Channel.Infrastructure.ZLink;

public sealed class PlayerActor(IZLinkActorContext context, PlayerData data, long gold)
    : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;
    public GoldBalance Gold { get; } = new(gold);

    public async ValueTask EarnAsync(long amount, CancellationToken ct)
    {
        var next = Gold.AfterEarning(amount);
        await data.SaveAsync(Context.ActorId, next, ct);
        Gold.Restore(next);
    }

    public ValueTask OnJoinCompletedAsync(ZLinkActorJoinCompletion completion, CancellationToken ct)
    {
        if (completion is ZLinkActorJoinCompletion.Failed failed)
            throw new InvalidOperationException($"Zone join failed: {failed.Kind}.");
        if (completion is ZLinkActorJoinCompletion.Rejected)
            throw new InvalidOperationException("Zone join rejected.");
        return ValueTask.CompletedTask;
    }
}

public sealed class PlayerFactory(PlayerData data) : IZLinkActorFactory<PlayerActor>
{
    public async ValueTask<PlayerActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken ct
    ) => new(context, data, await data.LoadAsync(context.ActorId, ct));
}

public sealed class PlayerRelocation : IZLinkActorRelocationAdapter<PlayerActor>
{
    public ValueTask<byte[]> CaptureAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.FromResult(JsonSerializer.SerializeToUtf8Bytes(actor.Gold.Value));

    public ValueTask RestoreAsync(
        PlayerActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken ct
    )
    {
        actor.Gold.Restore(JsonSerializer.Deserialize<long>(payload.Span));
        return ValueTask.CompletedTask;
    }
}
