using DeliveryDispatch.Server.Configuration;
using DeliveryDispatch.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Handlers;

namespace DeliveryDispatch.Server.Dispatch;

[ZLinkHandlerGroup(SampleNames.DispatchChannel)]
internal sealed class AssignDeliveryHandler(
    DispatchWorkQueue queue,
    ILogger<AssignDeliveryHandler> logger)
    : IZLinkSendHandler<AssignDeliveryMsg>
{
    public async ValueTask HandleAsync(
        AssignDeliveryMsg message,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        await queue.EnqueueAsync(message, cancellationToken);
        logger.LogInformation(
            "deliverydispatch dispatch-channel: enqueued delivery={DeliveryId} customer={CustomerId}",
            message.DeliveryId,
            message.CustomerId);
    }
}
