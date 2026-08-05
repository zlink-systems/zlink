using ShoppingMall.Server.Shared.Domain;
using ShoppingMall.Shared.Contracts;

namespace ShoppingMall.Server.Shared.Ports.Outbound;

public interface IOrderEventStore
{
    ValueTask<IReadOnlyList<StoredOrderEvent>> ReadAsync(
        string orderId,
        CancellationToken cancellationToken);

    ValueTask AppendAsync(
        string orderId,
        long expectedVersion,
        IReadOnlyList<OrderDomainEvent> events,
        CancellationToken cancellationToken);
}

public interface IOrderReadModelStore
{
    ValueTask<OrderProjectionState?> FindAsync(
        string orderId,
        CancellationToken cancellationToken);

    ValueTask SaveAsync(
        OrderProjectionState state,
        CancellationToken cancellationToken);

    ValueTask DeleteAsync(
        string orderId,
        CancellationToken cancellationToken);
}

public interface ICommerceStateStore
{
    ValueTask<CartSeed> GetCartAsync(
        string cartId,
        CancellationToken cancellationToken);

    ValueTask ValidateShippingAddressAsync(
        string shippingAddressId,
        CancellationToken cancellationToken);

    ValueTask<PaymentMethodSeed> GetPaymentMethodAsync(
        string paymentMethodId,
        CancellationToken cancellationToken);

    ValueTask<IdempotencyMapping?> FindIdempotencyAsync(
        string idempotencyKey,
        CancellationToken cancellationToken);

    ValueTask<IdempotencyMapping> ReserveIdempotencyAsync(
        string idempotencyKey,
        CancellationToken cancellationToken);

    ValueTask MarkIdempotencyStartedAsync(
        string idempotencyKey,
        CancellationToken cancellationToken);

    ValueTask CreatePendingMappingAsync(
        string idempotencyKey,
        string orderId,
        CancellationToken cancellationToken);

    ValueTask SaveOrderPaymentMethodAsync(
        string orderId,
        string paymentMethodId,
        CancellationToken cancellationToken);

    ValueTask<string> GetOrderPaymentMethodAsync(
        string orderId,
        CancellationToken cancellationToken);

    ValueTask<ReserveInventoryResult> ReserveInventoryAsync(
        ReserveInventoryCommand command,
        CancellationToken cancellationToken);

    ValueTask<ReleaseInventoryResult> ReleaseInventoryAsync(
        ReleaseInventoryCommand command,
        CancellationToken cancellationToken);

    ValueTask<AuthorizePaymentResult> AuthorizePaymentAsync(
        AuthorizePaymentCommand command,
        CancellationToken cancellationToken);
}

public sealed record StoredOrderEvent(
    string EventId,
    string? SourceCommandId,
    string OrderId,
    string EventType,
    byte[] Payload,
    long Version,
    long CreatedAtUnixMs);

public static class StoredOrderEventPayload
{
    public static byte[] Encode(OrderDomainEvent domainEvent) =>
        System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(
            domainEvent,
            domainEvent.GetType());

    public static OrderDomainEvent Decode(this StoredOrderEvent storedEvent)
    {
        var type = storedEvent.EventType switch
        {
            nameof(OrderStartedEvent) => typeof(OrderStartedEvent),
            nameof(InventoryReservedEvent) => typeof(InventoryReservedEvent),
            nameof(InventoryReservationFailedEvent) => typeof(InventoryReservationFailedEvent),
            nameof(PaymentAuthorizedEvent) => typeof(PaymentAuthorizedEvent),
            nameof(PaymentFailedEvent) => typeof(PaymentFailedEvent),
            nameof(InventoryReleasedEvent) => typeof(InventoryReleasedEvent),
            nameof(OrderConfirmedEvent) => typeof(OrderConfirmedEvent),
            nameof(OrderFailedEvent) => typeof(OrderFailedEvent),
            _ => throw new InvalidOperationException(
                $"Unsupported stored order event type '{storedEvent.EventType}'.")
        };
        return (OrderDomainEvent?)System.Text.Json.JsonSerializer.Deserialize(
                   storedEvent.Payload,
                   type)
               ?? throw new InvalidOperationException(
                   $"Stored order event '{storedEvent.EventId}' has an empty payload.");
    }
}

public sealed record IdempotencyMapping(
    string IdempotencyKey,
    string OrderId,
    bool Started);

public sealed record StoreEvidence(
    IReadOnlyDictionary<string, string[]> EventsByOrder,
    int PaymentFailureCount,
    int ReleasedReservationCount,
    int StartedIdempotencyCount);

public sealed class OrderStreamVersionConflictException(
    string orderId,
    long expectedVersion,
    long actualVersion)
    : Exception(
        $"Order stream version mismatch. order={orderId}, expected={expectedVersion}, actual={actualVersion}")
{
    public string OrderId { get; } = orderId;

    public long ExpectedVersion { get; } = expectedVersion;

    public long ActualVersion { get; } = actualVersion;
}
