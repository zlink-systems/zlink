using ShoppingMall.Shared.Contracts;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace ShoppingMall.Server.OrderWorkflow.Infrastructure.ZLink.Spots.OrderWorkflowSpot.Handlers;

internal sealed class ContinueOrderWorkflowHandler
    : IZLinkSpotRequestHandler<
        OrderWorkflowSpot,
        ContinueOrderWorkflowReq,
        ContinueOrderWorkflowRes
    >
{
    public ValueTask<ContinueOrderWorkflowRes> HandleAsync(
        OrderWorkflowSpot spot,
        IZLinkMessageContext context,
        ContinueOrderWorkflowReq request,
        CancellationToken cancellationToken
    )
    {
        return spot.ContinueOrderWorkflowAsync(request, cancellationToken);
    }
}
