namespace ShoppingMall.Server.Shared.Domain;

public abstract record OrderDomainEvent(
    string EventId,
    string OrderId,
    long CreatedAtUnixMs);

public sealed record OrderStartedEvent(
    string EventId,
    string SourceCommandId,
    string OrderId,
    string CartId,
    string ShippingAddressId,
    OrderLine[] Lines,
    decimal Amount,
    string Currency,
    long CreatedAtUnixMs) : OrderDomainEvent(EventId, OrderId, CreatedAtUnixMs);

public sealed record InventoryReservedEvent(
    string EventId,
    string OrderId,
    string ReservationId,
    long CreatedAtUnixMs) : OrderDomainEvent(EventId, OrderId, CreatedAtUnixMs);

public sealed record InventoryReservationFailedEvent(
    string EventId,
    string OrderId,
    string Reason,
    long CreatedAtUnixMs) : OrderDomainEvent(EventId, OrderId, CreatedAtUnixMs);

public sealed record PaymentAuthorizedEvent(
    string EventId,
    string OrderId,
    string PaymentId,
    long CreatedAtUnixMs) : OrderDomainEvent(EventId, OrderId, CreatedAtUnixMs);

public sealed record PaymentFailedEvent(
    string EventId,
    string OrderId,
    string Reason,
    long CreatedAtUnixMs) : OrderDomainEvent(EventId, OrderId, CreatedAtUnixMs);

public sealed record InventoryReleasedEvent(
    string EventId,
    string OrderId,
    string ReservationId,
    string Reason,
    long CreatedAtUnixMs) : OrderDomainEvent(EventId, OrderId, CreatedAtUnixMs);

public sealed record OrderConfirmedEvent(
    string EventId,
    string OrderId,
    long ConfirmedAtUnixMs) : OrderDomainEvent(EventId, OrderId, ConfirmedAtUnixMs);

public sealed record OrderFailedEvent(
    string EventId,
    string OrderId,
    string Reason,
    long FailedAtUnixMs) : OrderDomainEvent(EventId, OrderId, FailedAtUnixMs);

public sealed record ReserveInventoryResult(
    bool Accepted,
    string? ReservationId,
    string? Reason);

public sealed record AuthorizePaymentResult(
    bool Accepted,
    string? PaymentId,
    string? Reason);

public sealed record ReserveInventoryCommand(
    string OrderId,
    string ReservationId,
    IReadOnlyList<OrderLine> Lines);

public sealed record ReleaseInventoryCommand(
    string OrderId,
    string ReservationId,
    string Reason);

public sealed record ReleaseInventoryResult(bool Released);

public sealed record AuthorizePaymentCommand(
    string OrderId,
    string PaymentId,
    string PaymentMethodId,
    decimal Amount,
    string Currency);

public sealed record OrderLine(string Sku, int Quantity);

public enum OrderStatus
{
    Created,
    InventoryReserved,
    PaymentAuthorized,
    PaymentFailed,
    InventoryReleased,
    Confirmed,
    Failed
}

public sealed record OrderProjectionState(
    string OrderId,
    OrderStatus Status,
    string? ShippingAddressId,
    string? ReservationId,
    string? PaymentId,
    string? Reason,
    decimal? Amount,
    string? Currency,
    long UpdatedAtUnixMs);
