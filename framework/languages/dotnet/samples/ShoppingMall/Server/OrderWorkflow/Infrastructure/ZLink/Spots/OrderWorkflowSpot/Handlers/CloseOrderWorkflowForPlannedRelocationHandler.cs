using ShoppingMall.Shared.Contracts;
using Zlink.Framework.Contracts.Spots;

namespace ShoppingMall.Server.OrderWorkflow.Infrastructure.ZLink.Spots.OrderWorkflowSpot.Handlers;

internal sealed class CloseOrderWorkflowForPlannedRelocationHandler :
    IZLinkSpotRequestHandler<
        OrderWorkflowSpot,
        CloseOrderWorkflowForPlannedRelocationReq,
        CloseOrderWorkflowForPlannedRelocationRes>
{
    public ValueTask<CloseOrderWorkflowForPlannedRelocationRes> HandleAsync(
        OrderWorkflowSpot spot,
        CloseOrderWorkflowForPlannedRelocationReq request,
        CancellationToken cancellationToken)
    {
        return spot.CloseForPlannedRelocationAsync(request, cancellationToken);
    }
}
