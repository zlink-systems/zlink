using System.Text.Json;
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
    private readonly RoomState state = new();
    private readonly RoomRoster roster = new();

    public void Configure()
    {
        Context.Handlers.AddActorPacket<RoomReconnectHandler, PlayerActor>(nameof(JoinRoomRequest));
        Context.Handlers.AddActorPacket<RoomViewHandler, PlayerActor>(nameof(RoomViewRequest));
        Context.Handlers.AddActorPacket<MoveHandler, PlayerActor>(nameof(MoveRequest));
        Context.Handlers.AddActorPacket<ShootHandler, PlayerActor>(nameof(ShootRequest));
        Context.Handlers.AddActorPacket<RoomViewDelivery, PlayerActor>(nameof(RoomView));
        Context.Handlers.AddActorPacket<MatchFoundDelivery, PlayerActor>(nameof(MatchFound));
        Context.Handlers.AddActorPacket<MatchEndedDelivery, PlayerActor>(nameof(MatchEnded));
    }

    public RoomPrepared Prepare(PrepareRoom request) => roster.Prepare(Context.SpotId, request);

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string id,
        ZLinkMessage request,
        CancellationToken ct
    )
    {
        var admission = request.Decode<JoinRoomRequest>();
        if (admission.PlayerId != id || admission.RoomId != Context.SpotId)
            throw new InvalidDataException("Room admission identity mismatch.");
        return ValueTask.FromResult(
            roster.CanAdmit(id) && state.Admit(id)
                ? ZLinkSpotActorJoinResult.Accept() : ZLinkSpotActorJoinResult.Reject()
        );
    }

    public async ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct)
    {
        await actors.SendToActor(actor.Context.ActorId, new MatchFound(Context.SpotId)).Async();
        await BroadcastAsync(View(), ct);
    }

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    private RoomView View() => new(Context.SpotId, settings.Id, state.View(), state.Phase, state.Winner);

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
        var rejected = state.Move(actor.Context.ActorId, request);
        if (rejected is not null)
            await actor.Context.BoundSession.Send(rejected).Async();
        else
        {
            await actor.Context.BoundSession.Send(new MoveResult(
                state.Players.Single(p => p.PlayerId == actor.Context.ActorId).Position,
                request.Sequence)).Async();
            await BroadcastAsync(View(), ct);
        }
    }

    public async ValueTask ShootAsync(PlayerActor actor, ShootRequest request, CancellationToken ct)
    {
        var decision = state.Shoot(actor.Context.ActorId, request);
        if (decision.Rejected is { } rejected)
        {
            await actor.Context.BoundSession.Send(rejected).Async();
            return;
        }
        await actor.Context.BoundSession.Send(decision.Result!).Async();
        await BroadcastAsync(View(), ct);
        if (state.Winner is { } winner)
            await BroadcastAsync(new MatchEnded(winner), ct);
    }

    public byte[] Capture() => JsonSerializer.SerializeToUtf8Bytes(new HttpRoomSnapshot(state.Capture(), roster.Capture()));

    public void Restore(ReadOnlyMemory<byte> payload)
    {
        var snapshot = JsonSerializer.Deserialize<HttpRoomSnapshot>(payload.Span)
            ?? throw new InvalidDataException("Missing room state.");
        roster.Restore(snapshot.Roster);
        state.Restore(snapshot.Game);
    }
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
