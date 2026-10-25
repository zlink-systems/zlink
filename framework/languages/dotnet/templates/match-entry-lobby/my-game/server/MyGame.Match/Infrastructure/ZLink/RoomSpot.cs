using MyGame.Match.Domain;
using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Match;

public sealed class RoomSpot(
    IZLinkSpotContext context,
    NodeSettings settings,
    IZLinkActorClient actors
) : IZLinkSpot<PlayerActor>
{
    public IZLinkSpotContext Context { get; } = context;
    private RoomState state = new();

    public void Configure()
    {
        Context.Handlers.AddActorPacket<RoomReconnectHandler, PlayerActor>(nameof(QueueRequest));
        Context.Handlers.AddActorPacket<RoomViewHandler, PlayerActor>(nameof(RoomViewRequest));
        Context.Handlers.AddActorPacket<MoveHandler, PlayerActor>(nameof(MoveRequest));
        Context.Handlers.AddActorPacket<ShootHandler, PlayerActor>(nameof(ShootRequest));
        Context.Handlers.AddActorPacket<RoomViewDelivery, PlayerActor>(nameof(RoomView));
        Context.Handlers.AddActorPacket<MatchFoundDelivery, PlayerActor>(nameof(MatchFound));
        Context.Handlers.AddActorPacket<MatchEndedDelivery, PlayerActor>(nameof(MatchEnded));
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string id,
        ZLinkMessage request,
        CancellationToken ct
    )
    {
        var admission = request.Decode<RoomAdmission>();
        if (admission.PlayerId != id || admission.RoomId != Context.SpotId)
            throw new InvalidDataException("Room admission identity mismatch.");
        return ValueTask.FromResult(
            state.Admit(id) ? ZLinkSpotActorJoinResult.Accept() : ZLinkSpotActorJoinResult.Reject()
        );
    }

    public async ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct)
    {
        await actors.SendToActor(actor.Context.ActorId, new MatchFound(Context.SpotId)).Async();
        await BroadcastAsync(View(), ct);
    }

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    private RoomView View() =>
        new(Context.SpotId, settings.Id, state.Phase, state.Winner, state.View());

    public ValueTask ViewAsync(PlayerActor actor) =>
        actor.Context.BoundSession.Send(View()).Async();

    private async ValueTask BroadcastAsync<T>(T message, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        foreach (var player in state.Players)
            await actors.SendToActor(player.PlayerId, message).Async();
    }

    public async ValueTask MoveAsync(PlayerActor actor, MoveRequest request, CancellationToken ct)
    {
        var rejection = state.Move(actor.Context.ActorId, request);
        if (rejection is not null)
            await actor.Context.BoundSession.Send(rejection).Async();
        else
            await BroadcastAsync(View(), ct);
    }

    public async ValueTask ShootAsync(PlayerActor actor, ShootRequest request, CancellationToken ct)
    {
        var rejection = state.Shoot(actor.Context.ActorId, request);
        if (rejection is not null)
        {
            await actor.Context.BoundSession.Send(rejection).Async();
            return;
        }
        await BroadcastAsync(View(), ct);
        if (state.Winner is { } winner)
            await BroadcastAsync(new MatchEnded(winner), ct);
    }

    public byte[] Capture() => state.Capture();

    public void Restore(ReadOnlyMemory<byte> payload) => state.Restore(payload);
}

public sealed class RoomRelocation : IZLinkSpotRelocationAdapter<RoomSpot>
{
    public ValueTask<byte[]> CaptureAsync(RoomSpot spot, CancellationToken ct) =>
        ValueTask.FromResult(spot.Capture());

    public ValueTask RestoreAsync(RoomSpot spot, ReadOnlyMemory<byte> payload, CancellationToken ct)
    {
        spot.Restore(payload);
        return ValueTask.CompletedTask;
    }
}
