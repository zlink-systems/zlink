using ObservabilityOps.Server.Play.Infrastructure;
using ObservabilityOps.Server.Play.Support;
using ObservabilityOps.Shared;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace ObservabilityOps.Server.Play.Spots;

internal sealed class PlayEntrySpot(IZLinkEntrySpotContext context, EvidenceStore evidence)
    : IZLinkEntrySpot<PlayerActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        PlayerActor actor,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"actor-created|actor={actor.ActorId}|node={Context.NodeRid}");
        return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(string actorId, ZLinkMessage request,
        CancellationToken cancellationToken) =>
        ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));

    public async ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        evidence.Add($"actor-entry-joined|actor={actor.ActorId}|node={Context.NodeRid}|previous-room={actor.Player.RoomRid}");
        await actor.Context.BoundSession.Send(new PlayerMovedNotify(actor.ActorId, Context.NodeRid.ToString()))
            .Async(cancellationToken);
    }

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        cleanupCancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"spot-closing|kind=entry|spot={Context.SpotId}"
            + $"|node={Context.NodeRid}|reason={context.Reason}");
        return ValueTask.CompletedTask;
    }
}
