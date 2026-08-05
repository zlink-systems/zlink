using Bingo.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;

namespace Bingo.Server.Play.Infrastructure.ZLink.Actors;

internal sealed class PlayerActor(
    string actorId,
    IZLinkActorContext context,
    ILogger<PlayerActor> logger) : IZLinkActor
{
    private readonly Queue<(string RoomId, bool ObserveOnly)> _pendingJoins = new();

    public string DisplayName { get; private set; } = actorId;

    public string RoomId { get; private set; } = string.Empty;

    public bool DestroyAfterEntrySpotJoin { get; private set; }

    public bool Disconnected { get; private set; }
    public string ActorId { get; } = actorId;

    public IZLinkActorContext Context { get; } = context;

    public ActorRef? CurrentRef { get; private set; }

    public ZLinkActorJoinOperationId? LastCompletedJoinOperationId { get; private set; }

    public string LastCompletedJoinOutcome { get; private set; } = string.Empty;

    public void SetDisplayName(string displayName)
    {
        DisplayName = displayName;
    }

    public void JoinRoom(string roomId)
    {
        RoomId = roomId;
    }

    public void MarkForDestroyAfterRoomLeave()
    {
        DestroyAfterEntrySpotJoin = true;
    }

    public void MarkDisconnected()
    {
        Disconnected = true;
    }

    public void TrackDeferredJoin(string roomId, bool observeOnly)
    {
        _pendingJoins.Enqueue((roomId, observeOnly));
    }

    public IReadOnlyList<(string RoomId, bool ObserveOnly)> PendingJoinsSnapshot()
    {
        return _pendingJoins.ToArray();
    }

    public void RestorePendingJoins(
        IEnumerable<(string RoomId, bool ObserveOnly)> pendingJoins)
    {
        _pendingJoins.Clear();
        foreach (var pending in pendingJoins)
            _pendingJoins.Enqueue(pending);
    }

    public async ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var operationId = completion switch
        {
            ZLinkActorJoinCompletion.Accepted value => value.OperationId,
            ZLinkActorJoinCompletion.Rejected value => value.OperationId,
            ZLinkActorJoinCompletion.Failed value => value.OperationId,
            _ => throw new InvalidOperationException("Unknown Actor Join completion.")
        };
        if (LastCompletedJoinOperationId == operationId)
        {
            logger.LogInformation(
                "actor join duplicate ignored. actor={ActorId}, operation={OperationId}",
                ActorId,
                operationId);
            return;
        }
        logger.LogInformation(
            "actor join completed. actor={ActorId}, outcome={Outcome}",
            ActorId,
            completion.GetType().Name);
        var pending = _pendingJoins.Count > 0
            ? _pendingJoins.Peek()
            : ResolveRecoveredJoin(completion);

        switch (completion)
        {
            case ZLinkActorJoinCompletion.Accepted accepted
                when accepted.Reply is { } reply:
                CurrentRef = accepted.Actor;
                JoinRoom(pending.RoomId);
                if (pending.ObserveOnly)
                {
                    await Context.BoundSession.Send(new ObserveBingoEventsRes
                        {
                            Subscribed = true
                        })
                        .Async(cancellationToken);
                }
                else
                {
                    var joined = reply.Decode<BingoRoomJoinRes>();
                    await Context.BoundSession.Send(new MatchBingoRes
                        {
                            RoomId = pending.RoomId,
                            State = joined.State
                        })
                        .Async(cancellationToken);
                }
                if (_pendingJoins.Count > 0) _pendingJoins.Dequeue();
                RememberJoinCompletion(operationId, "Accepted");
                return;

            case ZLinkActorJoinCompletion.Rejected:
                logger.LogWarning(
                    "actor join rejected. actor={ActorId}, room={RoomId}",
                    ActorId,
                    pending.RoomId);
                if (_pendingJoins.Count > 0) _pendingJoins.Dequeue();
                RememberJoinCompletion(operationId, "Rejected");
                return;

            case ZLinkActorJoinCompletion.Failed failed:
                logger.LogWarning(
                    "actor join failed. actor={ActorId}, room={RoomId}, kind={Kind}",
                    ActorId,
                    pending.RoomId,
                    failed.Kind);
                if (_pendingJoins.Count > 0) _pendingJoins.Dequeue();
                RememberJoinCompletion(operationId, failed.Kind.ToString());
                return;
        }
    }

    public void RestoreJoinCompletion(
        ZLinkActorJoinOperationId? operationId,
        string outcome)
    {
        LastCompletedJoinOperationId = operationId;
        LastCompletedJoinOutcome = outcome;
    }

    private void RememberJoinCompletion(
        ZLinkActorJoinOperationId operationId,
        string outcome)
    {
        LastCompletedJoinOperationId = operationId;
        LastCompletedJoinOutcome = outcome;
    }

    private static (string RoomId, bool ObserveOnly) ResolveRecoveredJoin(
        ZLinkActorJoinCompletion completion)
    {
        var reply = completion switch
        {
            ZLinkActorJoinCompletion.Accepted { Reply: { } value } => value,
            _ => throw new InvalidOperationException(
                "Recovered Bingo Actor Join completion has no accepted reply.")
        };
        var state = reply.Decode<BingoRoomJoinRes>().State;
        return (state.RoomId, false);
    }

}
