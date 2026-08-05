using ShoppingMall.Server.Shared.Domain;

namespace ShoppingMall.Server.OrderWorkflow.Domain.ShoppingMall;

internal sealed class OrderAggregate
{
    private readonly HashSet<string> _sourceCommands = new(StringComparer.Ordinal);

    public string OrderId { get; private set; } = string.Empty;

    public OrderStatus? Status { get; private set; }

    public string? ReservationId { get; private set; }

    public bool HasStarted => !string.IsNullOrWhiteSpace(OrderId);

    public bool IsTerminal => Status is OrderStatus.Confirmed or OrderStatus.Failed;

    public bool HasProcessedMsg(string sourceCommandId)
    {
        return _sourceCommands.Contains(sourceCommandId);
    }

    public IReadOnlyList<OrderDomainEvent> Start(
        string sourceCommandId,
        string orderId,
        string cartId,
        string shippingAddressId,
        OrderLine[] lines,
        decimal amount,
        string currency,
        string eventId,
        long nowUnixMs)
    {
        if (HasStarted) return [];

        return
        [
            new OrderStartedEvent(
                eventId,
                sourceCommandId,
                orderId,
                cartId,
                shippingAddressId,
                lines,
                amount,
                currency,
                nowUnixMs)
        ];
    }

    public IReadOnlyList<OrderDomainEvent> ApplyInventoryResult(
        ReserveInventoryResult result,
        string eventId,
        string failureEventId,
        long nowUnixMs)
    {
        if (IsTerminal || Status != OrderStatus.Created) return [];

        if (!result.Accepted)
        {
            var reason = result.Reason ?? "inventory unavailable";
            return
            [
                new InventoryReservationFailedEvent(eventId, OrderId, reason, nowUnixMs),
                new OrderFailedEvent(failureEventId, OrderId, reason, nowUnixMs)
            ];
        }

        return
        [
            new InventoryReservedEvent(
                eventId,
                OrderId,
                result.ReservationId ?? throw new InvalidOperationException("Accepted reservation requires an id."),
                nowUnixMs)
        ];
    }

    public IReadOnlyList<OrderDomainEvent> ApplyPaymentResult(
        AuthorizePaymentResult result,
        string eventId,
        long nowUnixMs)
    {
        if (IsTerminal || Status != OrderStatus.InventoryReserved) return [];

        if (!result.Accepted)
        {
            var reason = result.Reason ?? "payment failed";
            return [new PaymentFailedEvent(eventId, OrderId, reason, nowUnixMs)];
        }

        return
        [
            new PaymentAuthorizedEvent(
                eventId,
                OrderId,
                result.PaymentId ?? throw new InvalidOperationException("Accepted payment requires an id."),
                nowUnixMs)
        ];
    }

    public IReadOnlyList<OrderDomainEvent> Confirm(string eventId, long nowUnixMs)
    {
        if (IsTerminal || Status != OrderStatus.PaymentAuthorized) return [];

        return [new OrderConfirmedEvent(eventId, OrderId, nowUnixMs)];
    }

    public IReadOnlyList<OrderDomainEvent> ReleaseInventory(
        string eventId,
        long nowUnixMs)
    {
        if (IsTerminal || Status != OrderStatus.PaymentFailed) return [];

        return
        [
            new InventoryReleasedEvent(
                eventId,
                OrderId,
                ReservationId ?? throw new InvalidOperationException("Reservation is required for compensation."),
                "payment failed",
                nowUnixMs)
        ];
    }

    public IReadOnlyList<OrderDomainEvent> FailAfterInventoryRelease(
        string eventId,
        long nowUnixMs)
    {
        if (IsTerminal || Status != OrderStatus.InventoryReleased) return [];

        return [new OrderFailedEvent(eventId, OrderId, "payment failed", nowUnixMs)];
    }

    public void Apply(OrderDomainEvent domainEvent)
    {
        switch (domainEvent)
        {
            case OrderStartedEvent started:
                OrderId = started.OrderId;
                Status = OrderStatus.Created;
                _sourceCommands.Add(started.SourceCommandId);
                break;
            case InventoryReservedEvent reserved:
                ReservationId = reserved.ReservationId;
                Status = OrderStatus.InventoryReserved;
                break;
            case InventoryReservationFailedEvent:
                Status = OrderStatus.Failed;
                break;
            case PaymentAuthorizedEvent:
                Status = OrderStatus.PaymentAuthorized;
                break;
            case PaymentFailedEvent:
                Status = OrderStatus.PaymentFailed;
                break;
            case InventoryReleasedEvent:
                Status = OrderStatus.InventoryReleased;
                break;
            case OrderConfirmedEvent:
                Status = OrderStatus.Confirmed;
                break;
            case OrderFailedEvent:
                Status = OrderStatus.Failed;
                break;
        }
    }

    public static OrderAggregate Rehydrate(IEnumerable<OrderDomainEvent> events)
    {
        var aggregate = new OrderAggregate();
        foreach (var domainEvent in events) aggregate.Apply(domainEvent);

        return aggregate;
    }
}
