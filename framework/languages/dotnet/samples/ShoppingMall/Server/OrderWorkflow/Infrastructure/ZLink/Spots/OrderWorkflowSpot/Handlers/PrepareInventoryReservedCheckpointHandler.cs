using ShoppingMall.Shared.Contracts;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace ShoppingMall.Server.OrderWorkflow.Infrastructure.ZLink.Spots.OrderWorkflowSpot.Handlers;

internal sealed class PrepareInventoryReservedCheckpointHandler
    : IZLinkSpotRequestHandler<
        OrderWorkflowSpot,
        PrepareInventoryReservedCheckpointReq,
        StartOrderWorkflowRes
    >
{
    public ValueTask<StartOrderWorkflowRes> HandleAsync(
        OrderWorkflowSpot spot,
        IZLinkMessageContext context,
        PrepareInventoryReservedCheckpointReq request,
        CancellationToken cancellationToken
    )
    {
        return spot.PrepareInventoryReservedCheckpointAsync(request, cancellationToken);
    }
}
