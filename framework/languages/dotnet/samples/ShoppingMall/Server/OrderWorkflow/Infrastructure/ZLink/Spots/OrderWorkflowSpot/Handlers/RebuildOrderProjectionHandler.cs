using ShoppingMall.Shared.Contracts;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace ShoppingMall.Server.OrderWorkflow.Infrastructure.ZLink.Spots.OrderWorkflowSpot.Handlers;

internal sealed class RebuildOrderProjectionHandler
    : IZLinkSpotRequestHandler<
        OrderWorkflowSpot,
        RebuildOrderProjectionReq,
        RebuildOrderProjectionRes
    >
{
    public ValueTask<RebuildOrderProjectionRes> HandleAsync(
        OrderWorkflowSpot spot,
        IZLinkMessageContext context,
        RebuildOrderProjectionReq request,
        CancellationToken cancellationToken
    )
    {
        return spot.RebuildOrderProjectionAsync(request, cancellationToken);
    }
}
