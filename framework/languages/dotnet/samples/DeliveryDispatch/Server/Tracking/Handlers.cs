using DeliveryDispatch.Server.Configuration;
using DeliveryDispatch.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;

namespace DeliveryDispatch.Server.Tracking;

[ZLinkHandlerGroup(SampleNames.TrackingRouteChannel)]
internal sealed class DeliveryStatusChangedHandler(
    EvidenceStore evidence,
    IZLinkActorClient actors,
    ILogger<DeliveryStatusChangedHandler> logger)
    : IZLinkRequestHandler<DeliveryStatusChangedReq, DeliveryStatusChangedRes>
{
    public async ValueTask<DeliveryStatusChangedRes> HandleAsync(
        DeliveryStatusChangedReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        evidence.Append(request);
        var updated = new DeliveryStatusUpdatedMsg(
            request.DeliveryId,
            request.CustomerId,
            request.Status,
            request.CourierId,
            request.OccurredAtUnixMs);
        await actors.SendToActor(request.CustomerId, updated)
            .Async(cancellationToken);
        logger.LogInformation(
            "deliverydispatch tracking: status delivery={DeliveryId} status={Status} courier={CourierId}",
            request.DeliveryId,
            request.Status,
            request.CourierId);
        return new DeliveryStatusChangedRes(request.DeliveryId, request.Status);
    }
}
