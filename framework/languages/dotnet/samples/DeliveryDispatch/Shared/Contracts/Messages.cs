namespace DeliveryDispatch.Shared.Contracts;

using System.Text.Json.Serialization;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;

[JsonConverter(typeof(JsonStringEnumConverter<DeliveryStatus>))]
public enum DeliveryStatus
{
    Created,
    Assigned,
    Accepted,
    Reassigned,
    PickedUp,
    Delivered,
    Failed
}

public sealed record CreateDeliveryReq(
    string DeliveryId,
    string CustomerId,
    string PickupAddress,
    string DropoffAddress);

public sealed record CreateDeliveryRes(
    string DeliveryId);

public sealed record EnsureCustomerActorReq(
    string CustomerId);

public sealed record BindCourierSessionReq(
    string CourierId);

public sealed record BindCourierSessionRes(
    string CourierId);

public sealed record EnsureCourierActorReq(
    string CourierId);

public sealed record SubscribeDeliveryReq(
    string DeliveryId);

public sealed record SubscribeDeliveryRes(
    string DeliveryId);

public sealed record AssignDeliveryMsg(
    string DeliveryId,
    string CustomerId,
    string PickupAddress,
    string DropoffAddress);

/// <summary>
/// The offer, and the courier's answer to it. Both are one-way (common sample spec §7.4): a
/// courier looks at a screen and presses a button, and tying that time to a request's reply
/// would hold the spot's serial queue open for as long as the courier thinks. The server cannot
/// request anything of a client anyway — it can only push — so the decision was always going to
/// arrive as a separate inbound message. <c>Attempt</c> is what makes the pairing safe: a
/// decision that names an attempt other than the current one arrived too late and is dropped.
/// </summary>
public sealed record OfferDeliveryMsg(
    string CourierId,
    string DeliveryId,
    int Attempt,
    string PickupAddress,
    string DropoffAddress);

public sealed record OfferDeliveryNotify(
    string CourierId,
    string DeliveryId,
    string PickupAddress,
    string DropoffAddress);

public sealed record OfferDeliveryResultMsg(
    string DeliveryId,
    string CourierId,
    int Attempt,
    bool Accepted,
    string? Reason);

public sealed record CourierDecisionMsg(
    string DeliveryId,
    string CourierId,
    bool Accepted,
    string? Reason);

public sealed record DeliveryStatusChangedReq(
    string DeliveryId,
    string CustomerId,
    DeliveryStatus Status,
    string? CourierId,
    long OccurredAtUnixMs);

public sealed record DeliveryStatusChangedRes(
    string DeliveryId,
    DeliveryStatus Status);

public sealed record DeliveryStatusNotify(
    string DeliveryId,
    DeliveryStatus Status,
    string? CourierId,
    long OccurredAtUnixMs);

public sealed record DeliveryStatusUpdatedMsg(
    string DeliveryId,
    string CustomerId,
    DeliveryStatus Status,
    string? CourierId,
    long OccurredAtUnixMs);
