namespace ShoppingMall.Shared.Contracts;

public sealed record StartOrderReq(
    string CartId,
    string ShippingAddressId,
    string PaymentMethodId,
    string IdempotencyKey);

public sealed record StartOrderRes(
    string OrderId,
    OrderState State);

public sealed record GetOrderStateReq(string OrderId);

public sealed record GetOrderStateRes(OrderState State);

public sealed record OrderState(
    string OrderId,
    string Status,
    string? ShippingAddressId,
    string? ReservationId,
    string? PaymentId,
    string? Reason,
    decimal? Amount,
    string? Currency,
    long UpdatedAtUnixMs);

public sealed record OrderLineInput(
    string Sku,
    int Quantity);

public sealed record CartSeed(
    string CartId,
    OrderLineInput[] Lines,
    decimal Amount,
    string Currency);

public sealed record InventorySeed(
    string Sku,
    int AvailableQuantity);

public sealed record PaymentMethodSeed(
    string PaymentMethodId,
    bool ShouldAuthorize,
    string? FailureReason);

public sealed record StartOrderWorkflowReq(
    string OrderId,
    string CartId,
    string ShippingAddressId,
    string PaymentMethodId,
    string IdempotencyKey,
    OrderLineInput[] Lines,
    decimal Amount,
    string Currency);

public sealed record StartOrderWorkflowRes(OrderState State);

public sealed record ContinueOrderWorkflowReq(string OrderId);

public sealed record ContinueOrderWorkflowRes(OrderState State);

public sealed record RebuildOrderProjectionReq(string OrderId);

public sealed record RebuildOrderProjectionRes(OrderState State);

public sealed record PrepareInventoryReservedCheckpointReq(StartOrderWorkflowReq Command);

public sealed record OrderProjectionUpdatedEvent(
    string OrderId,
    string Status);
