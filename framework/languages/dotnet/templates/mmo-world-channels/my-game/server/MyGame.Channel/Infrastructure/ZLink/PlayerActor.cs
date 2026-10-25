using System.Text.Json;
using MyGame.Channel.Domain;
using MyGame.Channel.Infrastructure.Redis;
using Zlink.Framework.Contracts.Actors;

namespace MyGame.Channel.Infrastructure.ZLink;

public sealed class PlayerActor(IZLinkActorContext context, PlayerData data, long gold)
    : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;
    public GoldBalance Gold { get; } = new(gold, TimeProvider.System);

    public async ValueTask<GoldEarnDecision> EarnAsync(CancellationToken ct)
    {
        var decision = Gold.EvaluateEarn();
        if (!decision.Accepted)
            return decision;

        var next = decision.NewValue!.Value;
        await data.SaveAsync(Context.ActorId, next, ct);
        Gold.CommitEarn(decision);
        return decision;
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
        ValueTask.FromResult(JsonSerializer.SerializeToUtf8Bytes(actor.Gold.Capture()));

    public ValueTask RestoreAsync(
        PlayerActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken ct
    )
    {
        var snapshot = JsonSerializer.Deserialize<GoldSnapshot>(payload.Span)
            ?? throw new InvalidDataException("Gold relocation state is empty.");
        actor.Gold.Restore(snapshot);
        return ValueTask.CompletedTask;
    }
}
