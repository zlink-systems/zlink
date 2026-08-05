using Bingo.Server.Play.Infrastructure.ZLink.Actors;
using Bingo.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace Bingo.Server.Play.Infrastructure.ZLink.Spots.EntrySpot;

internal sealed class BingoEntrySpot(
    IZLinkEntrySpotContext context,
    ILogger<BingoEntrySpot> logger) : IZLinkEntrySpot<PlayerActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        PlayerActor actor,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken)
    {
        var request = createRequest.Decode<EnsurePlayerActorReq>();
        actor.SetDisplayName(request.DisplayName);
        logger.LogInformation(
            "entry spot: actor created. actor={ActorId}, displayName={DisplayName}",
            actor.ActorId,
            actor.DisplayName);
        return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));
    }

    public async ValueTask OnJoinedActorAsync(
        PlayerActor actor,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "entry spot: actor joined. actor={ActorId}, destroyAfterJoin={DestroyAfterJoin}",
            actor.ActorId,
            actor.DestroyAfterEntrySpotJoin);
        if (actor.DestroyAfterEntrySpotJoin)
        {
            logger.LogInformation(
                "entry spot: actor destroy requested. actor={ActorId}",
                actor.ActorId);
            // Finished-room cleanup is server lifecycle work and must complete even
            // when the client closes immediately after receiving the final result.
            await Context.DestroyActorAsync(actor, CancellationToken.None);
            logger.LogInformation(
                "entry spot: actor destroy completed. actor={ActorId}",
                actor.ActorId);
            return;
        }

    }

    public ValueTask OnLeaveActorAsync(
        PlayerActor actor,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "entry spot: actor left. actor={ActorId}",
            actor.ActorId);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectActorAsync(
        PlayerActor actor,
        CancellationToken cancellationToken)
    {
        actor.MarkDisconnected();
        logger.LogInformation(
            "entry spot: actor disconnected. actor={ActorId}",
            actor.ActorId);
        return ValueTask.CompletedTask;
    }
}
