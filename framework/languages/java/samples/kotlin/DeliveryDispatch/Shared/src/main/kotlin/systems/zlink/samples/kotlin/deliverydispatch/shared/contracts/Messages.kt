package systems.zlink.samples.kotlin.deliverydispatch.shared.contracts

import java.time.Instant
import systems.zlink.framework.actors.ActorRefSnapshot

data class CreateDeliveryReq(
    val deliveryId: String,
    val customerId: String,
    val pickupAddress: String,
    val dropoffAddress: String,
)

data class CreateDeliveryRes(
    val deliveryId: String,
)

data class SubscribeDeliveryReq(
    val deliveryId: String,
)

data class SubscribeDeliveryRes(
    val deliveryId: String,
)

data class DeliveryStatusNotify(
    val deliveryId: String,
    val status: DeliveryStatus,
    val courierId: String,
    val occurredAt: Instant,
)

data class AssignDeliveryMsg(
    val deliveryId: String,
    val customerId: String,
    val pickupAddress: String,
    val dropoffAddress: String,
)

data class BindCourierSessionReq(
    val courierId: String,
    val actor: ActorRefSnapshot? = null,
    val sessionRoute: String? = null,
)

data class BindCourierSessionRes(
    val courierId: String,
    val actor: ActorRefSnapshot,
    val sessionRoute: String,
)

data class BindCourierReq(
    val courierId: String,
    val sessionRoute: String,
)

data class BindCourierRes(
    val courierId: String,
    val actor: ActorRefSnapshot,
    val sessionRoute: String,
)

data class FindCourierActorReq(
    val courierId: String,
)

data class FindCourierActorRes(
    val courierId: String,
    val actor: ActorRefSnapshot?,
)

data class EnsureCourierActorReq(
    val courierId: String,
)

data class EnsureCourierActorRes(
    val courierId: String,
    val actor: ActorRefSnapshot,
)

/**
 * The offer, and the courier's answer to it. Both are one-way (common sample spec section 7.4): a
 * courier looks at a screen and presses a button, and tying that time to a request's reply would
 * hold the spot's serial queue open for as long as the courier thinks. The decision was always
 * going to arrive as a separate inbound message, because the server can only push to a client,
 * never request of it. [attempt] is what makes the pairing safe: a decision that names another
 * attempt arrived too late and is dropped.
 */
data class OfferDeliveryMsg(
    val courierId: String,
    val deliveryId: String,
    val attempt: Int,
    val pickupAddress: String,
    val dropoffAddress: String,
)

data class OfferDeliveryResultMsg(
    val deliveryId: String,
    val courierId: String,
    val attempt: Int,
    val accepted: Boolean,
    val reason: String? = null,
)

data class OfferDeliveryNotify(
    val courierId: String,
    val deliveryId: String,
    val pickupAddress: String,
    val dropoffAddress: String,
)

data class CourierDecisionMsg(
    val deliveryId: String,
    val courierId: String,
    val accepted: Boolean,
    val reason: String? = null,
)

data class DeliveryStatusChangedReq(
    val deliveryId: String,
    val customerId: String,
    val status: DeliveryStatus,
    val courierId: String,
    val occurredAt: Instant,
)

data class DeliveryStatusUpdatedMsg(
    val deliveryId: String,
    val customerId: String,
    val status: DeliveryStatus,
    val courierId: String,
    val occurredAt: Instant,
)

data class DeliveryStatusChangedRes(
    val deliveryId: String,
    val status: DeliveryStatus,
)

data class FindCustomerActorReq(
    val customerId: String,
)

data class FindCustomerActorRes(
    val customerId: String,
    val actorRef: ActorRefSnapshot?,
)

data class ServerAssertionReq(
    val successfulDeliveryId: String,
    val reassignedDeliveryId: String,
)

data class ServerAssertionRes(
    val passed: Boolean,
    val evidence: Array<String>,
)

data class EnsureCustomerActorReq(
    val customerId: String,
)

data class EnsureCustomerActorRes(
    val customerId: String,
    val actorRef: ActorRefSnapshot,
)

enum class DeliveryStatus {
    Created,
    Assigned,
    Reassigned,
    Accepted,
    PickedUp,
    Delivered,
    Failed,
}
