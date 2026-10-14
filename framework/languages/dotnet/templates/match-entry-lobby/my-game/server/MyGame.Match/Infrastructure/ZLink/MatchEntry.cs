using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;
namespace MyGame.Match;

public sealed class MatchEntry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<PlayerActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;
    public void Configure() => Context.Handlers.AddActorPacket<EnterQueueHandler, PlayerActor>(nameof(QueueRequest));
    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct) => ValueTask.CompletedTask;
    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) => ValueTask.CompletedTask;
}
public sealed class EnterQueueHandler : IZLinkEntrySpotActorSendHandler<MatchEntry, PlayerActor, QueueRequest>
{
    public ValueTask HandleAsync(MatchEntry spot, PlayerActor actor, IZLinkMessageContext context, QueueRequest request, CancellationToken ct)
    {
        if (actor.Context.ActorId != request.PlayerId) throw new InvalidDataException("Queue identity mismatch.");
        actor.Context.JoinSpot(GameNames.MatchmakingId, request).Defer();
        return ValueTask.CompletedTask;
    }
}
