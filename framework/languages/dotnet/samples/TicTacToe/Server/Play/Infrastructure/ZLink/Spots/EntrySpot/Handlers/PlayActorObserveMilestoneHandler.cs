using TicTacToe.Server.Play.Infrastructure.ZLink.Actors;
using TicTacToe.Shared.Contracts;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace TicTacToe.Server.Play.Infrastructure.ZLink.Spots.EntrySpot.Handlers;

[ZLinkSpotActorRequestHandler(nameof(ObserveMilestoneReq))]
internal sealed class PlayActorObserveMilestoneHandler(
    ILogger<PlayActorObserveMilestoneHandler> logger)
    : IZLinkEntrySpotActorRequestHandler<PlayEntrySpot, PlayActor, ObserveMilestoneReq, ObserveMilestoneRes>
{
    public async ValueTask<ObserveMilestoneRes> HandleAsync(
        PlayEntrySpot entrySpot,
        PlayActor actor,
        IZLinkMessageContext context,
        ObserveMilestoneReq message,
        CancellationToken cancellationToken)
    {
        _ = message;

        logger.LogInformation(
            "actor: ObserveMilestoneReq received. actor={ActorId}",
            actor.ActorId);

        await entrySpot.SubscribeMilestoneAsync(actor, cancellationToken);

        logger.LogInformation(
            "actor: ObserveMilestoneReq completed. actor={ActorId}, subscribed={Subscribed}",
            actor.ActorId,
            true);
        return new ObserveMilestoneRes(true);
    }
}
