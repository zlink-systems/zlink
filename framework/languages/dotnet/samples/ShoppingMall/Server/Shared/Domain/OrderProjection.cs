namespace ShoppingMall.Server.Shared.Domain;

public static class OrderProjection
{
    public static OrderProjectionState Apply(
        OrderProjectionState? current,
        OrderDomainEvent domainEvent)
    {
        return domainEvent switch
        {
            OrderStartedEvent started => new OrderProjectionState(
                started.OrderId,
                OrderStatus.Created,
                started.ShippingAddressId,
                null,
                null,
                null,
                started.Amount,
                started.Currency,
                started.CreatedAtUnixMs),
            InventoryReservedEvent reserved => Ensure(current, reserved).With(
                OrderStatus.InventoryReserved,
                reserved.ReservationId,
                UpdatedAtUnixMs: reserved.CreatedAtUnixMs),
            InventoryReservationFailedEvent failed => Ensure(current, failed).With(
                OrderStatus.Failed,
                Reason: failed.Reason,
                UpdatedAtUnixMs: failed.CreatedAtUnixMs),
            PaymentAuthorizedEvent paid => Ensure(current, paid).With(
                OrderStatus.PaymentAuthorized,
                PaymentId: paid.PaymentId,
                UpdatedAtUnixMs: paid.CreatedAtUnixMs),
            PaymentFailedEvent failed => Ensure(current, failed).With(
                OrderStatus.Failed,
                Reason: failed.Reason,
                UpdatedAtUnixMs: failed.CreatedAtUnixMs),
            InventoryReleasedEvent released => Ensure(current, released).With(
                Reason: released.Reason,
                UpdatedAtUnixMs: released.CreatedAtUnixMs),
            OrderConfirmedEvent confirmed => Ensure(current, confirmed).With(
                OrderStatus.Confirmed,
                UpdatedAtUnixMs: confirmed.ConfirmedAtUnixMs),
            OrderFailedEvent failed => Ensure(current, failed).With(
                OrderStatus.Failed,
                Reason: failed.Reason,
                UpdatedAtUnixMs: failed.FailedAtUnixMs),
            _ => throw new InvalidOperationException($"Unsupported order event '{domainEvent.GetType()}'.")
        };
    }

    private static OrderProjectionState Ensure(
        OrderProjectionState? current,
        OrderDomainEvent domainEvent)
    {
        return current ?? throw new InvalidOperationException(
            $"Projection cannot apply '{domainEvent.GetType().Name}' before OrderStartedEvent.");
    }

    private static OrderProjectionState With(
        this OrderProjectionState current,
        OrderStatus? Status = null,
        string? ReservationId = null,
        string? PaymentId = null,
        string? Reason = null,
        long? UpdatedAtUnixMs = null)
    {
        return current with
        {
            Status = Status ?? current.Status,
            ReservationId = ReservationId ?? current.ReservationId,
            PaymentId = PaymentId ?? current.PaymentId,
            Reason = Reason ?? current.Reason,
            UpdatedAtUnixMs = UpdatedAtUnixMs ?? current.UpdatedAtUnixMs
        };
    }
}
