using AutomaticTurnDispatch.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace AutomaticTurnDispatch.Server.Play.Spots;

internal sealed class AwaitEntrySpot(IZLinkEntrySpotContext context) : IZLinkEntrySpot<AwaitActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken) =>
        ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));

    public ValueTask OnJoinedActorAsync(AwaitActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(AwaitActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}

internal sealed class AwaitActorFactory(EvidenceStore evidence) : IZLinkActorFactory<AwaitActor>
{
    public ValueTask<AwaitActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new AwaitActor(context.ActorId, context, evidence));
    }
}

internal sealed class AwaitActor(
    string actorId,
    IZLinkActorContext context,
    EvidenceStore evidence) : IZLinkActor
{
    private readonly Queue<(string RequestId, string Marker)> _pendingJoins = new();

    public string ActorId { get; } = actorId;

    public IZLinkActorContext Context { get; } = context;

    public void TrackJoin(string requestId, string marker)
    {
        _pendingJoins.Enqueue((requestId, marker));
    }

    public ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var pending = _pendingJoins.Count == 0
            ? ResolveRelocatedJoin(completion)
            : _pendingJoins.Dequeue();
        var accepted = completion is ZLinkActorJoinCompletion.Accepted;
        evidence.Add(
            $"{pending.Marker}-resumed|rid={evidence.Rid}|spot={Context.SpotId}"
            + $"|actor={ActorId}|request={pending.RequestId}|accepted={accepted}");
        evidence.Add(
            $"{pending.Marker}-completed|rid={evidence.Rid}|spot={Context.SpotId}"
            + $"|actor={ActorId}|request={pending.RequestId}|accepted={accepted}");
        return ValueTask.CompletedTask;
    }

    private static (string RequestId, string Marker) ResolveRelocatedJoin(
        ZLinkActorJoinCompletion completion)
    {
        if (completion is ZLinkActorJoinCompletion.Accepted
            {
                Reply: { IsEmpty: false } reply
            })
        {
            var request = reply.Decode<JoinDelayReq>();
            return (request.RequestId, request.CompletionMarker);
        }

        return (RequestId: "unknown", Marker: "actor-join");
    }
}
