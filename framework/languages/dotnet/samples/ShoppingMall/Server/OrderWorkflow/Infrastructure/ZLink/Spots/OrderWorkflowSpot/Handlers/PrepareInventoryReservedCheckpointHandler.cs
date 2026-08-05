using ShoppingMall.Shared.Contracts;
using Zlink.Framework.Contracts.Spots;

namespace ShoppingMall.Server.OrderWorkflow.Infrastructure.ZLink.Spots.OrderWorkflowSpot.Handlers;

internal sealed class PrepareInventoryReservedCheckpointHandler :
    IZLinkSpotRequestHandler<OrderWorkflowSpot, PrepareInventoryReservedCheckpointReq, StartOrderWorkflowRes>
{
    public ValueTask<StartOrderWorkflowRes> HandleAsync(
        OrderWorkflowSpot spot,
        PrepareInventoryReservedCheckpointReq request,
        CancellationToken cancellationToken)
    {
        return spot.PrepareInventoryReservedCheckpointAsync(request, cancellationToken);
    }
}
