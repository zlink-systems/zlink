using System.Text.Json.Serialization;

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
    [property: JsonConverter(typeof(NullableDecimalNumberJsonConverter))] decimal? Amount,
    string? Currency,
    long UpdatedAtUnixMs);

public sealed record OrderLineInput(
    string Sku,
    int Quantity);

public sealed record CartSeed(
    string CartId,
    OrderLineInput[] Lines,
    [property: JsonConverter(typeof(DecimalNumberJsonConverter))] decimal Amount,
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
    string SourceCommandId,
    OrderLineInput[] Lines,
    [property: JsonConverter(typeof(DecimalNumberJsonConverter))] decimal Amount,
    string Currency);

public sealed record StartOrderWorkflowRes(OrderState State);

public sealed record ContinueOrderWorkflowReq(
    string OrderId,
    string SourceCommandId);

public sealed record ContinueOrderWorkflowRes(OrderState State);

public sealed record RebuildOrderProjectionReq(
    string OrderId,
    string SourceCommandId);

public sealed record RebuildOrderProjectionRes(OrderState State);

public sealed record PrepareInventoryReservedCheckpointReq(StartOrderWorkflowReq Command);

public sealed record CloseOrderWorkflowForPlannedRelocationReq;

public sealed record CloseOrderWorkflowForPlannedRelocationRes(bool Closed);

public sealed record StartPlannedRelocationReq;

public sealed record StartPlannedRelocationRes(bool Started);

public sealed record SignalPlannedRelocationReadyReq;

public sealed record SignalPlannedRelocationReadyRes(bool Deferred);

public sealed record OrderProjectionUpdatedEvent(
    string OrderId,
    string Status);
