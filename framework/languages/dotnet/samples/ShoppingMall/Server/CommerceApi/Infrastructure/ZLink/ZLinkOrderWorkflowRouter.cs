using ShoppingMall.Server.CommerceApi.Ports.Outbound;
using ShoppingMall.Server.Configuration;
using ShoppingMall.Shared.Contracts;
using Zlink.Framework.Contracts.Spots;

namespace ShoppingMall.Server.CommerceApi.Infrastructure.ZLink;

internal sealed class ZLinkOrderWorkflowRouter(
    IZLinkSpotClient spots) : IOrderWorkflowRouter
{
    public async ValueTask<OrderState> StartAsync(
        StartOrderWorkflowReq command,
        CancellationToken cancellationToken)
    {
        var response = await Request(command.OrderId, command)
            .Async<StartOrderWorkflowRes>(cancellationToken);
        return response.State;
    }

    public async ValueTask<OrderState> ContinueAsync(
        ContinueOrderWorkflowReq command,
        CancellationToken cancellationToken)
    {
        var response = await Request(command.OrderId, command)
            .Async<ContinueOrderWorkflowRes>(cancellationToken);
        return response.State;
    }

    public async ValueTask<OrderState> RebuildProjectionAsync(
        RebuildOrderProjectionReq command,
        CancellationToken cancellationToken)
    {
        var response = await Request(command.OrderId, command)
            .Async<RebuildOrderProjectionRes>(cancellationToken);
        return response.State;
    }

    public async ValueTask<OrderState> PrepareInventoryReservedCheckpointAsync(
        StartOrderWorkflowReq command,
        CancellationToken cancellationToken)
    {
        var response = await Request(
                command.OrderId,
                new PrepareInventoryReservedCheckpointReq(command))
            .Async<StartOrderWorkflowRes>(cancellationToken);
        return response.State;
    }

    private IZLinkSpotRequestCall Request<TMessage>(
        string orderId,
        TMessage command) =>
        spots.RequestToSpot(orderId, command)
            // The first command cold-activates the order owner. Existing
            // orders use the authority already published for this SpotId.
            .InstanceSpot(SampleNames.OrderWorkflowSpotType)
            .InMesh(SampleNames.MeshName);
}
