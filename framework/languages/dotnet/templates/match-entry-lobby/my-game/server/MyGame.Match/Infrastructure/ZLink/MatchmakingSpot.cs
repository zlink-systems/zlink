using System.Text.Json;
using MyGame.Shared;
using MyGame.Match.Domain;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Handlers;
namespace MyGame.Match;

public sealed class MatchmakingSpot(IZLinkSpotContext context, IZLinkSpotManager spots, IZLinkActorClient actors) : IZLinkSpot<PlayerActor>
{
    public IZLinkSpotContext Context { get; } = context;
    private MatchmakingState state = new();
    public void Configure()
    {
        Context.Handlers.AddActorPacket<QueueAgainHandler, PlayerActor>(nameof(QueueRequest));
        Context.Handlers.AddActorPacket<AssignmentHandler, PlayerActor>(nameof(RoomAssignment));
    }
    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(string id, ZLinkMessage request, CancellationToken ct)
    {
        var queue = request.Decode<QueueRequest>();
        if (queue.PlayerId != id) throw new InvalidDataException("Queue identity mismatch.");
        state.Enqueue(queue);
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());
    }
    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct) => MatchAsync(ct);
    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) => ValueTask.CompletedTask;
    public async ValueTask QueueAsync(PlayerActor actor, QueueRequest request, CancellationToken ct)
    {
        if (actor.Context.ActorId != request.PlayerId) throw new InvalidDataException("Queue identity mismatch.");
        state.Enqueue(request);
        await MatchAsync(ct);
    }
    private async ValueTask MatchAsync(CancellationToken ct)
    {
        var pair = state.Pair();
        if (pair.Length == 0) return;
        var roomId = state.AllocateRoom();
        var created = await spots.GetOrCreate(roomId, GameNames.RoomType).InMesh(GameNames.WorldMesh).Async(ct);
        if (created.State == ZLinkSpotCreateState.Rejected) throw new InvalidOperationException("Room creation rejected.");
        state.Remove(pair);
        foreach (var player in pair)
            await actors.SendToActor(player.PlayerId, new RoomAssignment(roomId)).Async();
    }
    public byte[] Capture() => JsonSerializer.SerializeToUtf8Bytes(state);
    public void Restore(ReadOnlyMemory<byte> payload) => state = JsonSerializer.Deserialize<MatchmakingState>(payload.Span) ?? throw new InvalidDataException("Missing matchmaking state.");
}
public sealed class QueueAgainHandler : IZLinkSpotActorSendHandler<MatchmakingSpot, PlayerActor, QueueRequest>
{
    public ValueTask HandleAsync(MatchmakingSpot spot, PlayerActor actor, IZLinkMessageContext context, QueueRequest request, CancellationToken ct) => spot.QueueAsync(actor, request, ct);
}
public sealed class AssignmentHandler : IZLinkSpotActorSendHandler<MatchmakingSpot, PlayerActor, RoomAssignment>
{
    public ValueTask HandleAsync(MatchmakingSpot spot, PlayerActor actor, IZLinkMessageContext context, RoomAssignment message, CancellationToken ct)
    {
        actor.Context.JoinSpot(message.RoomId, new RoomAdmission(actor.Context.ActorId, message.RoomId)).Defer();
        return ValueTask.CompletedTask;
    }
}
public sealed class MatchmakingRelocation : IZLinkSpotRelocationAdapter<MatchmakingSpot>
{
    public ValueTask<byte[]> CaptureAsync(MatchmakingSpot spot, CancellationToken ct) => ValueTask.FromResult(spot.Capture());
    public ValueTask RestoreAsync(MatchmakingSpot spot, ReadOnlyMemory<byte> payload, CancellationToken ct)
    { spot.Restore(payload); return ValueTask.CompletedTask; }
}
