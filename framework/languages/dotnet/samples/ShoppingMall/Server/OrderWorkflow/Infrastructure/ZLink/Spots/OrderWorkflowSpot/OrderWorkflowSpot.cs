using ShoppingMall.Server.Configuration;
using ShoppingMall.Server.OrderWorkflow.Application.OrderWorkflow;
using ShoppingMall.Server.OrderWorkflow.Application.SelfCheck;
using ShoppingMall.Shared.Contracts;
using Zlink.Framework.Contracts.Spots;

namespace ShoppingMall.Server.OrderWorkflow.Infrastructure.ZLink.Spots.OrderWorkflowSpot;

internal sealed class OrderWorkflowSpot(
    IZLinkInstanceSpotContext context,
    OrderWorkflowService workflow,
    OrderWorkflowSelfCheckService selfChecks,
    ILogger<OrderWorkflowSpot> logger) : IZLinkInstanceSpot
{
    public IZLinkInstanceSpotContext Context { get; } = context;

    public ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        logger.LogInformation(
            "shoppingmall order spot: ready. order={OrderId}, spot={SpotId}",
            Context.SpotId,
            Context.SpotId);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        _ = context;
        cleanupCancellationToken.ThrowIfCancellationRequested();
        return ValueTask.CompletedTask;
    }

    public async ValueTask<StartOrderWorkflowRes> StartOrderWorkflowAsync(
        StartOrderWorkflowReq request,
        CancellationToken cancellationToken)
    {
        var state = await workflow.StartAndContinueAsync(request, cancellationToken);
        logger.LogInformation(
            "shoppingmall order: started. order={OrderId}, status={Status}",
            state.OrderId,
            state.Status);
        await CloseIfTerminalAsync(state, cancellationToken);
        return new StartOrderWorkflowRes(state);
    }

    public async ValueTask<ContinueOrderWorkflowRes> ContinueOrderWorkflowAsync(
        ContinueOrderWorkflowReq request,
        CancellationToken cancellationToken)
    {
        var state = await workflow.ContinueAsync(request, cancellationToken);
        logger.LogInformation(
            "shoppingmall order: continued. order={OrderId}, status={Status}",
            state.OrderId,
            state.Status);
        await CloseIfTerminalAsync(state, cancellationToken);
        return new ContinueOrderWorkflowRes(state);
    }

    public async ValueTask<StartOrderWorkflowRes> PrepareInventoryReservedCheckpointAsync(
        PrepareInventoryReservedCheckpointReq request,
        CancellationToken cancellationToken)
    {
        var state = await selfChecks.PrepareInventoryReservedCheckpointAsync(request.Command, cancellationToken);
        logger.LogInformation(
            "shoppingmall order: inventory reserved. order={OrderId}, status={Status}",
            state.OrderId,
            state.Status);
        await Context.Outbound.Publish(
                SampleNames.OrderProjectionChannel,
                SampleNames.OrderProjectionTopic,
                new OrderProjectionUpdatedEvent(
                    state.OrderId,
                    state.Status))
            .Async(cancellationToken);
        return new StartOrderWorkflowRes(state);
    }

    public async ValueTask<RebuildOrderProjectionRes> RebuildOrderProjectionAsync(
        RebuildOrderProjectionReq request,
        CancellationToken cancellationToken)
    {
        var state = await workflow.RebuildProjectionAsync(request.OrderId, cancellationToken);
        logger.LogInformation(
            "shoppingmall order: projection rebuilt. order={OrderId}, status={Status}",
            state.OrderId,
            state.Status);
        await CloseIfTerminalAsync(state, cancellationToken);
        return new RebuildOrderProjectionRes(state);
    }

    private async ValueTask CloseIfTerminalAsync(
        OrderState state,
        CancellationToken cancellationToken)
    {
        if (state.Status is OrderStatuses.Confirmed or OrderStatuses.Failed)
            await Context.CloseAsync(cancellationToken);
    }
}
