using ShoppingMall.Server.OrderWorkflow.Domain.ShoppingMall;
using ShoppingMall.Server.Shared.Contracts;
using ShoppingMall.Server.Shared.Domain;
using ShoppingMall.Server.Shared.Ports.Outbound;
using ShoppingMall.Shared.Contracts;

namespace ShoppingMall.Server.OrderWorkflow.Application.OrderWorkflow;

internal sealed class OrderWorkflowService(
    IOrderEventStore events,
    IOrderReadModelStore readModels,
    ICommerceStateStore commerce)
{
    public async ValueTask<OrderState> StartAsync(
        StartOrderWorkflowReq command,
        CancellationToken cancellationToken)
    {
        var stored = await events.ReadAsync(command.OrderId, cancellationToken);
        var aggregate = OrderAggregate.Rehydrate(stored.Select(static item => item.Decode()));
        if (aggregate.HasProcessedMsg(command.IdempotencyKey))
        {
            await commerce.MarkIdempotencyStartedAsync(command.IdempotencyKey, cancellationToken);
            return OrderContractMapper.ToContract(
                await SaveProjectionFromEventsAsync(stored, cancellationToken));
        }

        var now = NowUnixMs();
        var started = aggregate.Start(
            command.IdempotencyKey,
            command.OrderId,
            command.CartId,
            command.ShippingAddressId,
            command.Lines.Select(static line => new OrderLine(line.Sku, line.Quantity)).ToArray(),
            command.Amount,
            command.Currency,
            NewEventId("started", command.OrderId),
            now);
        if (started.Count > 0) await AppendAndProjectAsync(command.OrderId, stored.Count, started, cancellationToken);

        await commerce.MarkIdempotencyStartedAsync(command.IdempotencyKey, cancellationToken);
        return OrderContractMapper.ToContract(
            await RequireProjectionAsync(command.OrderId, cancellationToken));
    }

    public async ValueTask<OrderState> StartAndContinueAsync(
        StartOrderWorkflowReq command,
        CancellationToken cancellationToken)
    {
        var state = await StartAsync(command, cancellationToken);
        if (state.Status is nameof(OrderStatus.Confirmed) or nameof(OrderStatus.Failed)) return state;

        _ = ContinueWorkflowInBackgroundAsync(command.OrderId)
            .ContinueWith(
                static task => _ = task.Exception,
                CancellationToken.None,
                TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
        return state;
    }

    private async Task ContinueWorkflowInBackgroundAsync(string orderId)
    {
        for (var attempt = 0; attempt < 20; attempt++)
        {
            try
            {
                await ContinueAsync(new ContinueOrderWorkflowReq(orderId), CancellationToken.None)
                    .ConfigureAwait(false);
                return;
            }
            catch (OrderStreamVersionConflictException)
            {
                await Task.Delay(20, CancellationToken.None).ConfigureAwait(false);
            }
        }

        await ContinueAsync(new ContinueOrderWorkflowReq(orderId), CancellationToken.None)
            .ConfigureAwait(false);
    }

    public async ValueTask<OrderState> ContinueAsync(
        ContinueOrderWorkflowReq command,
        CancellationToken cancellationToken)
    {
        var state = await ContinueUntilAsync(
            command.OrderId,
            static status => status is OrderStatus.Confirmed or OrderStatus.Failed,
            cancellationToken);
        return OrderContractMapper.ToContract(state);
    }

    internal async ValueTask<OrderState> ContinueUntilInventoryReservedAsync(
        string orderId,
        CancellationToken cancellationToken)
    {
        var state = await ContinueUntilAsync(
            orderId,
            static status => status is OrderStatus.InventoryReserved
                or OrderStatus.Confirmed
                or OrderStatus.Failed,
            cancellationToken);
        return OrderContractMapper.ToContract(state);
    }

    private async ValueTask<OrderProjectionState> ContinueUntilAsync(
        string orderId,
        Func<OrderStatus?, bool> shouldStop,
        CancellationToken cancellationToken)
    {
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var stored = await events.ReadAsync(orderId, cancellationToken);
            var aggregate = OrderAggregate.Rehydrate(stored.Select(static item => item.Decode()));
            var current = await SaveProjectionFromEventsAsync(stored, cancellationToken);

            if (aggregate.IsTerminal || shouldStop(aggregate.Status)) return current;

            var next = aggregate.Status switch
            {
                OrderStatus.Created => aggregate.ApplyInventoryResult(
                    await commerce.ReserveInventoryAsync(
                        new ReserveInventoryCommand(
                            orderId,
                            ReservationId(orderId),
                            stored.OfTypeStored<OrderStartedEvent>().Single().Lines),
                        cancellationToken),
                    NewEventId("inventory", orderId),
                    NewEventId("failed", orderId),
                    NowUnixMs()),
                OrderStatus.InventoryReserved => await AuthorizePaymentAsync(
                    aggregate,
                    current,
                    await commerce.GetOrderPaymentMethodAsync(orderId, cancellationToken),
                    cancellationToken),
                OrderStatus.PaymentAuthorized => aggregate.Confirm(NewEventId("confirmed", orderId), NowUnixMs()),
                OrderStatus.PaymentFailed => await ReleaseInventoryAsync(
                    aggregate,
                    current,
                    cancellationToken),
                OrderStatus.InventoryReleased => aggregate.FailAfterInventoryRelease(
                    NewEventId("failed", orderId),
                    NowUnixMs()),
                _ => []
            };

            if (next.Count == 0) return current;

            await AppendAndProjectAsync(orderId, stored.Count, next, cancellationToken);
        }
    }

    public async ValueTask<OrderState> RebuildProjectionAsync(
        string orderId,
        CancellationToken cancellationToken)
    {
        var stored = await events.ReadAsync(orderId, cancellationToken);
        OrderProjectionState? state = null;
        foreach (var storedEvent in stored) state = OrderProjection.Apply(state, storedEvent.Decode());

        if (state is null) throw new InvalidOperationException($"Order '{orderId}' has no event stream.");

        await readModels.SaveAsync(state, cancellationToken);
        return OrderContractMapper.ToContract(state);
    }

    private async ValueTask<IReadOnlyList<OrderDomainEvent>> AuthorizePaymentAsync(
        OrderAggregate aggregate,
        OrderProjectionState current,
        string paymentMethodId,
        CancellationToken cancellationToken)
    {
        var result = await commerce.AuthorizePaymentAsync(
            new AuthorizePaymentCommand(
                current.OrderId,
                PaymentId(current.OrderId),
                paymentMethodId,
                current.Amount ?? throw new InvalidOperationException("Order amount is required."),
                current.Currency ?? throw new InvalidOperationException("Order currency is required.")),
            cancellationToken);
        return aggregate.ApplyPaymentResult(
            result,
            NewEventId("payment", current.OrderId),
            NowUnixMs());
    }

    private async ValueTask<IReadOnlyList<OrderDomainEvent>> ReleaseInventoryAsync(
        OrderAggregate aggregate,
        OrderProjectionState current,
        CancellationToken cancellationToken)
    {
        var reservationId = current.ReservationId
                            ?? throw new InvalidOperationException("Reservation is required for compensation.");
        var reason = current.Reason ?? "payment failed";
        _ = await commerce.ReleaseInventoryAsync(
            new ReleaseInventoryCommand(current.OrderId, reservationId, reason),
            cancellationToken);
        return aggregate.ReleaseInventory(
            NewEventId("release", current.OrderId),
            NowUnixMs());
    }

    private async ValueTask AppendAndProjectAsync(
        string orderId,
        long expectedVersion,
        IReadOnlyList<OrderDomainEvent> domainEvents,
        CancellationToken cancellationToken)
    {
        await events.AppendAsync(orderId, expectedVersion, domainEvents, cancellationToken);
        var current = await readModels.FindAsync(orderId, cancellationToken);
        foreach (var domainEvent in domainEvents)
        {
            current = OrderProjection.Apply(current, domainEvent);
            await readModels.SaveAsync(current, cancellationToken);
        }
    }

    private async ValueTask<OrderProjectionState> RequireProjectionAsync(
        string orderId,
        CancellationToken cancellationToken)
    {
        return await readModels.FindAsync(orderId, cancellationToken)
               ?? throw new InvalidOperationException($"Order projection '{orderId}' does not exist.");
    }

    private async ValueTask<OrderProjectionState> SaveProjectionFromEventsAsync(
        IReadOnlyList<StoredOrderEvent> stored,
        CancellationToken cancellationToken)
    {
        OrderProjectionState? state = null;
        foreach (var storedEvent in stored) state = OrderProjection.Apply(state, storedEvent.Decode());

        if (state is null)
            throw new InvalidOperationException("Order event stream is empty.");

        await readModels.SaveAsync(state, cancellationToken);
        return state;
    }

    private static string NewEventId(string prefix, string orderId)
    {
        return $"{prefix}-{orderId}-{Guid.NewGuid():N}";
    }

    private static string ReservationId(string orderId)
    {
        return $"reservation-{orderId}";
    }

    private static string PaymentId(string orderId)
    {
        return $"payment-{orderId}";
    }

    private static long NowUnixMs()
    {
        return DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
    }

}

internal static class StoredOrderEventExtensions
{
    public static IEnumerable<TEvent> OfTypeStored<TEvent>(this IEnumerable<StoredOrderEvent> events)
        where TEvent : OrderDomainEvent
    {
        return events.Select(static item => item.Decode()).OfType<TEvent>();
    }
}
