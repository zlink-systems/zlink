using ShoppingMall.Shared.Contracts;
using Zlink.Framework.Contracts.Spots;

namespace ShoppingMall.Server.OrderWorkflow.Infrastructure.ZLink.Spots.OrderWorkflowSpot.Handlers;

// --8<-- [start:doc-sm-start-handler]
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
// --8<-- [end:doc-sm-start-handler]