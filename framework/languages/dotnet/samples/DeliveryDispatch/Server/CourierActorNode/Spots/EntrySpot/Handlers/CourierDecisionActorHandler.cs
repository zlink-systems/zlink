using Zlink.Framework.Contracts.Handlers;
using DeliveryDispatch.Server.Configuration;
using DeliveryDispatch.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Spots;

namespace DeliveryDispatch.Server.CourierActorNode.Spots.EntrySpot.Handlers;

/// <summary>
/// The courier's decision, sent back to dispatch one-way. The node neither judges it nor times
/// it — it carries the attempt the offer was made under, and dispatch decides whether that
/// attempt is still the current one (common sample spec §7.4).
/// </summary>
internal sealed class CourierDecisionActorHandler(
    IZLinkRouteClient channels,
    ILogger<CourierDecisionActorHandler> logger)
    : IZLinkEntrySpotActorSendHandler<CourierEntrySpot, CourierActor, CourierDecisionMsg>
{
    public async ValueTask HandleAsync(
        CourierEntrySpot entrySpot,
        CourierActor actor,
        IZLinkMessageContext context,
        CourierDecisionMsg message,
        CancellationToken cancellationToken)
    {
        var attempt = actor.TakeOfferedAttempt(message.DeliveryId);
        if (attempt is null)
        {
            logger.LogWarning(
                "deliverydispatch courier-actor: decision for an unknown offer delivery={DeliveryId} courier={CourierId}",
                message.DeliveryId,
                actor.ActorId);
            return;
        }

        await channels
            .SendToChannel(SampleNames.DispatchChannel,
                new OfferDeliveryResultMsg(
                    message.DeliveryId,
                    message.CourierId,
                    attempt.Value,
                    message.Accepted,
                    message.Reason))
            .Async(cancellationToken);

        logger.LogInformation(
            "deliverydispatch courier-actor: decision delivery={DeliveryId} courier={CourierId} attempt={Attempt} accepted={Accepted}",
            message.DeliveryId,
            actor.ActorId,
            attempt.Value,
            message.Accepted);
    }
}
