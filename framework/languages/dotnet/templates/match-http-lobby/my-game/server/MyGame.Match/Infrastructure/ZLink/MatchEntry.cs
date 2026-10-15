using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Match;

public sealed class MatchEntry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<PlayerActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure() =>
        Context.Handlers.AddActorPacket<JoinRoomHandler, PlayerActor>(nameof(JoinRoomRequest));

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;
}

public sealed class JoinRoomHandler
    : IZLinkEntrySpotActorSendHandler<MatchEntry, PlayerActor, JoinRoomRequest>
{
    public ValueTask HandleAsync(
        MatchEntry spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinRoomRequest request,
        CancellationToken ct
    )
    {
        if (actor.Context.ActorId != request.PlayerId)
            throw new InvalidDataException("Join identity mismatch.");
        ArgumentException.ThrowIfNullOrWhiteSpace(request.RoomId);
        actor.Context.JoinSpot(request.RoomId, request).Defer();
        return ValueTask.CompletedTask;
    }
}
