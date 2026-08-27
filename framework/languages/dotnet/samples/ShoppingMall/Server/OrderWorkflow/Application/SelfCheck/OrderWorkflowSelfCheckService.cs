using ShoppingMall.Server.Configuration;
using ShoppingMall.Server.OrderWorkflow.Application.OrderWorkflow;
using ShoppingMall.Server.Shared.Ports.Outbound;
using ShoppingMall.Shared.Contracts;

namespace ShoppingMall.Server.OrderWorkflow.Application.SelfCheck;

internal sealed class OrderWorkflowSelfCheckService(
    OrderWorkflowService workflow,
    ICommerceStateStore commerce)
{
    public async ValueTask<OrderState> PrepareInventoryReservedCheckpointAsync(
        StartOrderWorkflowReq command,
        CancellationToken cancellationToken)
    {
        var started = await workflow.StartAsync(command, cancellationToken);
        return started.Status == OrderStatuses.Created
            ? await workflow.ContinueUntilInventoryReservedAsync(command.OrderId, cancellationToken)
            : started;
    }

    public ValueTask ArmPlannedRelocationReplayAsync(
        string orderId,
        CancellationToken cancellationToken) =>
        commerce.ArmPlannedRelocationReplayAsync(orderId, cancellationToken);

    public ValueTask<bool> TryConsumePlannedRelocationReplayAsync(
        string orderId,
        CancellationToken cancellationToken) =>
        commerce.TryConsumePlannedRelocationReplayAsync(orderId, cancellationToken);
}
