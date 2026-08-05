using ShoppingMall.Server.CommerceApi.Ports.Outbound;
using ShoppingMall.Server.Shared.Contracts;
using ShoppingMall.Server.Shared.Ports.Outbound;
using ShoppingMall.Shared.Contracts;

namespace ShoppingMall.Server.CommerceApi.Application.OrderWorkflow;

internal sealed class StartOrderUseCase(
    OrderStartPreparation preparation,
    ICommerceStateStore commerce,
    IOrderReadModelStore readModels,
    IOrderWorkflowRouter workflows)
{
    public async ValueTask<StartOrderRes> ExecuteAsync(
        StartOrderReq request,
        CancellationToken cancellationToken)
    {
        var existing = await commerce.FindIdempotencyAsync(request.IdempotencyKey, cancellationToken);
        if (existing is { Started: true })
        {
            var existingState = await readModels.FindAsync(existing.OrderId, cancellationToken)
                                ?? throw new InvalidOperationException(
                                    $"Started order '{existing.OrderId}' has no projection.");
            return new StartOrderRes(
                existingState.OrderId,
                OrderContractMapper.ToContract(existingState));
        }

        var cart = await preparation.LoadCartAndValidateAsync(request, cancellationToken);

        var mapping = existing ?? await commerce.ReserveIdempotencyAsync(
            request.IdempotencyKey,
            cancellationToken);
        var command = await preparation.BuildCommandAsync(request, mapping, cart, cancellationToken);
        var state = existing is null
            ? await workflows.StartAsync(command, cancellationToken)
            : await ReadOrStartAsync(mapping.OrderId, command, cancellationToken);
        return new StartOrderRes(state.OrderId, state);

        async ValueTask<OrderState> ReadOrStartAsync(
            string orderId,
            StartOrderWorkflowReq workflowCommand,
            CancellationToken token)
        {
            var projection = await readModels.FindAsync(orderId, token);
            return projection is null
                ? await workflows.StartAsync(workflowCommand, token)
                : OrderContractMapper.ToContract(projection);
        }
    }
}

internal sealed class OrderStartPreparation(ICommerceStateStore commerce)
{
    public async ValueTask<CartSeed> LoadCartAndValidateAsync(
        StartOrderReq request,
        CancellationToken cancellationToken)
    {
        var cart = await commerce.GetCartAsync(request.CartId, cancellationToken);
        await commerce.ValidateShippingAddressAsync(request.ShippingAddressId, cancellationToken);
        _ = await commerce.GetPaymentMethodAsync(request.PaymentMethodId, cancellationToken);
        return cart;
    }

    public async ValueTask<StartOrderWorkflowReq> BuildCommandAsync(
        StartOrderReq request,
        IdempotencyMapping mapping,
        CartSeed cart,
        CancellationToken cancellationToken)
    {
        await commerce.SaveOrderPaymentMethodAsync(
            mapping.OrderId,
            request.PaymentMethodId,
            cancellationToken);

        return new StartOrderWorkflowReq(
            mapping.OrderId,
            request.CartId,
            request.ShippingAddressId,
            request.PaymentMethodId,
            request.IdempotencyKey,
            cart.Lines,
            cart.Amount,
            cart.Currency);
    }
}

internal sealed class PrepareInventoryReservedOrderUseCase(
    OrderStartPreparation preparation,
    ICommerceStateStore commerce,
    IOrderWorkflowRouter workflows)
{
    public async ValueTask<StartOrderRes> ExecuteAsync(
        StartOrderReq request,
        CancellationToken cancellationToken)
    {
        var cart = await preparation.LoadCartAndValidateAsync(request, cancellationToken);
        var mapping = await commerce.ReserveIdempotencyAsync(
            request.IdempotencyKey,
            cancellationToken);
        var command = await preparation.BuildCommandAsync(request, mapping, cart, cancellationToken);
        var state = await workflows.PrepareInventoryReservedCheckpointAsync(command, cancellationToken);
        return new StartOrderRes(state.OrderId, state);
    }
}

internal sealed class GetOrderStateUseCase(
    IOrderReadModelStore readModels)
{
    public async ValueTask<GetOrderStateRes> ExecuteAsync(
        GetOrderStateReq request,
        CancellationToken cancellationToken)
    {
        var state = await readModels.FindAsync(request.OrderId, cancellationToken)
                    ?? throw new InvalidOperationException($"Order projection '{request.OrderId}' does not exist.");
        return new GetOrderStateRes(OrderContractMapper.ToContract(state));
    }
}
