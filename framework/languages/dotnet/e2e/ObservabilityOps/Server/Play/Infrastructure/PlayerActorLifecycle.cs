using System.Text.Json;
using ObservabilityOps.Server.Play.Domain;
using ObservabilityOps.Shared;
using Zlink.Framework.Contracts.Actors;

namespace ObservabilityOps.Server.Play.Infrastructure;

internal sealed class PlayerActor(
    Player player,
    IZLinkActorContext context) : IZLinkActor
{
    private const int ProcessedJoinOperationRetention = 256;
    private readonly Queue<PlayerPendingJoin> _pendingJoins = [];
    private readonly HashSet<ZLinkActorJoinOperationId> _processedJoinOperations = [];
    private readonly Queue<ZLinkActorJoinOperationId> _processedJoinOperationOrder = [];

    public IZLinkActorContext Context { get; } = context;

    public string ActorId => Context.ActorId;

    public Player Player { get; } = player;

    internal void TrackRoomJoin(string roomId)
    {
        _pendingJoins.Enqueue(new PlayerPendingJoin(roomId, false, string.Empty));
    }

    internal void TrackEntrySpotJoin(string marker)
    {
        _pendingJoins.Enqueue(new PlayerPendingJoin(null, true, marker));
    }

    internal PlayerPendingJoin[] SnapshotPendingJoins() => _pendingJoins.ToArray();

    internal ZLinkActorJoinOperationId[] SnapshotProcessedJoinOperations() =>
        _processedJoinOperationOrder.ToArray();

    internal void RestoreJoinState(
        IEnumerable<PlayerPendingJoin> pending,
        IEnumerable<ZLinkActorJoinOperationId> processed)
    {
        foreach (var item in pending) _pendingJoins.Enqueue(item);
        foreach (var operationId in processed) RememberJoinOperation(operationId);
    }

    public async ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var operationId = completion switch
        {
            ZLinkActorJoinCompletion.Accepted accepted => accepted.OperationId,
            ZLinkActorJoinCompletion.Rejected rejected => rejected.OperationId,
            ZLinkActorJoinCompletion.Failed failed => failed.OperationId,
            _ => throw new ArgumentOutOfRangeException(nameof(completion))
        };
        if (_processedJoinOperations.Contains(operationId)) return;

        var pending = _pendingJoins.Count > 0
            ? _pendingJoins.Peek()
            : RecoverPendingJoin(completion);
        var acceptedOutcome = completion is ZLinkActorJoinCompletion.Accepted;
        var nodeRid = completion is ZLinkActorJoinCompletion.Accepted moved
            ? moved.Actor.NodeRid.ToString()
            : string.Empty;
        var error = completion switch
        {
            ZLinkActorJoinCompletion.Rejected => "Rejected",
            ZLinkActorJoinCompletion.Failed failed => failed.Kind.ToString(),
            _ => null
        };

        if (acceptedOutcome)
        {
            if (pending.EntrySpot)
                Player.ReturnToLobby();
            else if (pending.SpotId is { } roomId)
                Player.JoinRoom(roomId);
        }

        await Context.BoundSession.Send(new ActorJoinCompletedNotify(
                Context.ActorId,
                pending.EntrySpot ? null : pending.SpotId,
                nodeRid,
                pending.Marker,
                acceptedOutcome,
                error))
            .Async(cancellationToken);

        RememberJoinOperation(operationId);
        if (_pendingJoins.Count > 0) _pendingJoins.Dequeue();
    }

    private void RememberJoinOperation(ZLinkActorJoinOperationId operationId)
    {
        if (!_processedJoinOperations.Add(operationId)) return;
        _processedJoinOperationOrder.Enqueue(operationId);
        while (_processedJoinOperationOrder.Count > ProcessedJoinOperationRetention)
            _processedJoinOperations.Remove(_processedJoinOperationOrder.Dequeue());
    }

    private static PlayerPendingJoin RecoverPendingJoin(
        ZLinkActorJoinCompletion completion)
    {
        if (completion is ZLinkActorJoinCompletion.Accepted { Reply: { } reply })
        {
            try
            {
                var room = reply.Decode<JoinRoomRes>();
                return new PlayerPendingJoin(room.RoomRid, false, string.Empty);
            }
            catch (Exception)
            {
                var lobby = reply.Decode<ReturnToLobbyReq>();
                return new PlayerPendingJoin(null, true, lobby.Marker);
            }
        }

        return new PlayerPendingJoin(null, false, string.Empty);
    }
}

internal sealed record PlayerPendingJoin(
    string? SpotId,
    bool EntrySpot,
    string Marker);

internal sealed record PlayerRelocationState(
    string RoomRid,
    PlayerPendingJoin[] PendingJoins,
    ZLinkActorJoinOperationId[] ProcessedJoinOperations);

internal sealed class PlayerActorFactory : IZLinkActorFactory<PlayerActor>
{
    public ValueTask<PlayerActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(
            new PlayerActor(new Player(context.ActorId), context));
    }
}

internal sealed class PlayerActorRelocationAdapter
    : IZLinkActorRelocationAdapter<PlayerActor>
{
    public ValueTask<byte[]> CaptureAsync(
        PlayerActor actor,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(JsonSerializer.SerializeToUtf8Bytes(
            new PlayerRelocationState(
                actor.Player.RoomRid,
                actor.SnapshotPendingJoins(),
                actor.SnapshotProcessedJoinOperations())));
    }

    public ValueTask RestoreAsync(
        PlayerActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var restored = JsonSerializer.Deserialize<PlayerRelocationState>(
            payload.Span) ?? throw new InvalidDataException(
            "Player relocation state is empty.");
        if (!string.IsNullOrWhiteSpace(restored.RoomRid))
            actor.Player.JoinRoom(restored.RoomRid);
        actor.RestoreJoinState(
            restored.PendingJoins ?? [],
            restored.ProcessedJoinOperations ?? []);
        return ValueTask.CompletedTask;
    }
}
