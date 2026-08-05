using TicTacToe.Shared.Contracts;
using Zlink.Framework.Contracts.Actors;

namespace TicTacToe.Server.Play.Infrastructure.ZLink.Actors;

internal sealed class PlayActor(
    string actorId,
    IZLinkActorContext context)
    : IZLinkActor
{
    private const int ProcessedJoinOperationRetention = 256;
    private readonly Queue<string> _pendingJoins = new();
    private readonly HashSet<ZLinkActorJoinOperationId> _processedJoinOperations = [];
    private readonly Queue<ZLinkActorJoinOperationId> _processedJoinOperationOrder = new();

    public string RoomId { get; private set; } = string.Empty;

    public PlayerInfo? Player { get; private set; }

    public bool DestroyAfterEntrySpotJoin { get; private set; }

    public bool Disconnected { get; private set; }
    public string ActorId { get; } = actorId;

    public IZLinkActorContext Context { get; } = context;

    public void ApplyPlayer(PlayerInfo player)
    {
        if (!string.Equals(player.ActorId, ActorId, StringComparison.Ordinal))
            throw new InvalidOperationException(
                $"Authenticated player '{player.ActorId}' does not match actor '{ActorId}'.");

        Player = player;
    }

    public PlayerInfo RequirePlayer()
    {
        return Player ?? throw new InvalidOperationException("Actor has not been authenticated.");
    }

    public void JoinRoom(string roomId)
    {
        if (string.IsNullOrWhiteSpace(roomId))
            throw new ArgumentException("Room id must not be empty.", nameof(roomId));

        RoomId = roomId;
    }

    public void TrackDeferredJoin(string roomId)
    {
        _pendingJoins.Enqueue(roomId);
    }

    internal IReadOnlyCollection<ZLinkActorJoinOperationId> ProcessedJoinOperations =>
        _processedJoinOperationOrder;

    internal void RestoreProcessedJoinOperations(
        IEnumerable<ZLinkActorJoinOperationId> operationIds)
    {
        foreach (var operationId in operationIds)
            RememberJoinOperation(operationId);
    }

    public async ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var operationId = GetOperationId(completion);
        if (_processedJoinOperations.Contains(operationId)) return;

        var roomId = _pendingJoins.Count > 0
            ? _pendingJoins.Peek()
            : ResolveRecoveredRoomId(completion);

        switch (completion)
        {
            case ZLinkActorJoinCompletion.Accepted { Reply: { } reply }:
                JoinRoom(roomId);
                await Context.BoundSession
                    .Send(new JoinGameRes(reply.Decode<TicTacToeGameJoinRes>().State))
                    .Async(cancellationToken);
                break;

            case ZLinkActorJoinCompletion.Rejected:
                await Context.BoundSession
                    .Send(new JoinGameFailedNotify(roomId, "Rejected"))
                    .Async(cancellationToken);
                break;

            case ZLinkActorJoinCompletion.Failed failed:
                await Context.BoundSession
                    .Send(new JoinGameFailedNotify(
                        roomId,
                        failed.Kind.ToString()))
                    .Async(cancellationToken);
                break;
        }

        RememberJoinOperation(operationId);
        if (_pendingJoins.Count > 0) _pendingJoins.Dequeue();
    }

    private bool RememberJoinOperation(ZLinkActorJoinOperationId operationId)
    {
        if (!_processedJoinOperations.Add(operationId)) return false;

        _processedJoinOperationOrder.Enqueue(operationId);
        while (_processedJoinOperationOrder.Count > ProcessedJoinOperationRetention)
            _processedJoinOperations.Remove(_processedJoinOperationOrder.Dequeue());
        return true;
    }

    private static ZLinkActorJoinOperationId GetOperationId(
        ZLinkActorJoinCompletion completion) =>
        completion switch
        {
            ZLinkActorJoinCompletion.Accepted accepted => accepted.OperationId,
            ZLinkActorJoinCompletion.Rejected rejected => rejected.OperationId,
            ZLinkActorJoinCompletion.Failed failed => failed.OperationId,
            _ => throw new ArgumentOutOfRangeException(nameof(completion))
        };

    private static string ResolveRecoveredRoomId(
        ZLinkActorJoinCompletion completion)
    {
        return completion switch
        {
            ZLinkActorJoinCompletion.Accepted { Reply: { } reply } =>
                reply.Decode<TicTacToeGameJoinRes>().State.RoomId,
            _ => string.Empty
        };
    }

    public string RequireJoinedRoom()
    {
        if (Context.SpotId is null || string.IsNullOrEmpty(RoomId))
            throw new InvalidOperationException("Actor has not joined a room.");

        return RoomId;
    }

    public void MarkForDestroyAfterRoomLeave()
    {
        DestroyAfterEntrySpotJoin = true;
    }

    public void MarkDisconnected()
    {
        Disconnected = true;
    }
}
