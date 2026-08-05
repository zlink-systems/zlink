using ShoppingMall.Shared.Contracts;
using Zlink.Framework.Contracts.Spots;

namespace ShoppingMall.Server.OrderWorkflow.Infrastructure.ZLink.Spots.OrderWorkflowSpot.Handlers;

internal sealed class StartOrderWorkflowHandler :
    IZLinkSpotRequestHandler<OrderWorkflowSpot, StartOrderWorkflowReq, StartOrderWorkflowRes>
{
    public ValueTask<StartOrderWorkflowRes> HandleAsync(
        OrderWorkflowSpot spot,
        StartOrderWorkflowReq request,
        CancellationToken cancellationToken)
    {
        return spot.StartOrderWorkflowAsync(request, cancellationToken);
    }
}