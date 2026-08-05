using DeliveryDispatch.Server.Configuration;
using DeliveryDispatch.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Handlers;

namespace DeliveryDispatch.Server.Dispatch;

/// <summary>
/// The courier's decision, arriving as its own inbound message rather than as the reply to a
/// request nobody could have made. A decision that names an attempt other than the one on
/// record came back after the offer had already been reassigned, and is dropped — that check is
/// what makes it safe never to wait (common sample spec §7.4).
/// </summary>
[ZLinkHandlerGroup(SampleNames.DispatchChannel)]
internal sealed class OfferDeliveryResultHandler(
    DeliveryOfferStore offers,
    DispatchWorker worker,
    ILogger<OfferDeliveryResultHandler> logger)
    : IZLinkSendHandler<OfferDeliveryResultMsg>
{
    public async ValueTask HandleAsync(
        OfferDeliveryResultMsg message,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        var offer = offers.Settle(message.DeliveryId, message.Attempt);
        if (offer is null)
        {
            logger.LogInformation(
                "deliverydispatch dispatch: dropped a late decision delivery={DeliveryId} attempt={Attempt}",
                message.DeliveryId,
                message.Attempt);
            return;
        }

        logger.LogInformation(
            "deliverydispatch dispatch: decision delivery={DeliveryId} courier={CourierId} attempt={Attempt} accepted={Accepted}",
            message.DeliveryId,
            message.CourierId,
            message.Attempt,
            message.Accepted);
        await worker.SettleAsync(offer, message.Accepted, message.Reason, cancellationToken);
    }
}
