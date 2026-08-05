using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using ZoneWorld.Server.ZoneNode.Domain.ZoneWorld;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Actors;

/// <summary>
/// The authority for one player's coordinate, zone and node (§2.1). The zone spot keeps
/// a copy for rendering, but this is the value that decides a move.
///
/// A bot is the same type. The only difference is that it has no bound session, so no
/// client push is ever addressed to it (§2.7).
/// </summary>
public sealed class PlayerActor(string actorId, IZLinkActorContext context) : IZLinkActor
{
    private const int ProcessedJoinOperationRetention = 256;
    private readonly Queue<PlayerPosition> _pendingJoins = new();
    private readonly HashSet<ZLinkActorJoinOperationId> _processedJoinOperations = [];
    private readonly Queue<ZLinkActorJoinOperationId> _processedJoinOperationOrder = new();

    public string ActorId { get; } = actorId;

    public IZLinkActorContext Context { get; } = context;

    public PlayerPosition Position { get; private set; }

    /// <summary>The zone the coordinate falls in. Derived, not stored: the authority for a
    /// player cannot be allowed to disagree with itself about where that player is (§2.1).</summary>
    public string ZoneId => Position.ZoneId;

    public bool IsBot { get; private set; }

    /// <summary>
    /// Records the player kind before an asynchronous entry or relocation join starts.
    /// A rejected deferred join still has to follow the bot policy even though the target
    /// Spot has not called <see cref="Restore"/> yet.
    /// </summary>
    public void PrepareEntry(bool isBot) => IsBot = isBot;

    /// <summary>Patrol direction. Meaningful only for a bot; it reverses when a move is
    /// rejected so the bot walks back the way it came (§2.7).</summary>
    public int DirX { get; private set; }

    public int DirY { get; private set; }

    /// <summary>
    /// Rebuilds the player on the node it relocated to (§2.6). The zone travels in the payload
    /// alongside the coordinate, so it is checked rather than trusted: if the two disagree, the
    /// relocation payload is corrupt, and a player silently teleporting is worse than a failed relocation.
    /// </summary>
    public void Restore(int x, int y, string zoneId, bool isBot, int dirX, int dirY)
    {
        var position = new PlayerPosition(x, y);
        if (position.ZoneId != zoneId)
            throw new InvalidOperationException(
                $"Relocated state for '{ActorId}' disagrees with itself: ({x},{y}) is in "
                + $"'{position.ZoneId}', but the state says '{zoneId}'.");

        Position = position;
        IsBot = isBot;
        DirX = dirX;
        DirY = dirY;
    }

    public void SetPatrol(int dirX, int dirY)
    {
        DirX = dirX;
        DirY = dirY;
    }

    public void MoveTo(PlayerPosition position) => Position = position;

    public void TrackDeferredJoin(PlayerPosition target)
    {
        _pendingJoins.Enqueue(target);
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

        switch (completion)
        {
            case ZLinkActorJoinCompletion.Accepted:
                break;

            case ZLinkActorJoinCompletion.Rejected { Reply: { } reply }:
                await NotifyJoinFailureAsync(
                    reply.Decode<EnterZoneRes>().Error ?? MoveRejectReasons.ZoneMaintenance,
                    cancellationToken);
                break;

            case ZLinkActorJoinCompletion.Rejected:
                await NotifyJoinFailureAsync(
                    MoveRejectReasons.ZoneMaintenance,
                    cancellationToken);
                break;

            case ZLinkActorJoinCompletion.Failed failed:
                await NotifyJoinFailureAsync(
                    failed.Kind.ToString(),
                    cancellationToken);
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

    private async ValueTask NotifyJoinFailureAsync(
        string reason,
        CancellationToken cancellationToken)
    {
        if (IsBot)
        {
            ReverseDirection();
            return;
        }

        await Context.BoundSession
            .Send(new MoveRejectedNotify(reason, Position.X, Position.Y))
            .Async(cancellationToken);
    }

    public void ReverseDirection()
    {
        DirX = -DirX;
        DirY = -DirY;
    }
}
