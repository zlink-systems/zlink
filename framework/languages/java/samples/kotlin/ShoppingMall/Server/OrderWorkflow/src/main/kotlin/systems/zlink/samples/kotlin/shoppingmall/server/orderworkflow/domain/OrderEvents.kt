package systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain

/**
 * Order domain events. Each event type maps to its simple class name, which is
 * also the `eventType` discriminator in the store and the sequence the
 * self-check assertion compares against.
 */
object OrderEventTypes {
    const val OrderStarted = "OrderStartedEvent"
    const val InventoryReserved = "InventoryReservedEvent"
    const val InventoryReservationFailed = "InventoryReservationFailedEvent"
    const val PaymentAuthorized = "PaymentAuthorizedEvent"
    const val PaymentFailed = "PaymentFailedEvent"
    const val InventoryReleased = "InventoryReleasedEvent"
    const val OrderConfirmed = "OrderConfirmedEvent"
    const val OrderFailed = "OrderFailedEvent"
}

object OrderStatus {
    const val Created = "Created"
    const val InventoryReserved = "InventoryReserved"
    const val PaymentAuthorized = "PaymentAuthorized"
    const val Confirmed = "Confirmed"
    const val Failed = "Failed"
}

data class OrderLine(val sku: String, val quantity: Int)

data class StartOrderCommand(
    val orderId: String,
    val cartId: String,
    val shippingAddressId: String,
    val paymentMethodId: String,
    val idempotencyKey: String,
    val lines: List<OrderLine>,
    val amount: Double,
    val currency: String,
)

data class OrderStartedEvent(
    val eventId: String,
    val sourceCommandId: String,
    val orderId: String,
    val cartId: String,
    val shippingAddressId: String,
    val paymentMethodId: String,
    val lines: List<OrderLine>,
    val amount: Double,
    val currency: String,
    val createdAtUnixMs: Long,
)

data class InventoryReservedEvent(
    val eventId: String,
    val orderId: String,
    val reservationId: String,
    val createdAtUnixMs: Long,
)

data class InventoryReservationFailedEvent(
    val eventId: String,
    val orderId: String,
    val reason: String,
    val createdAtUnixMs: Long,
)

data class PaymentAuthorizedEvent(
    val eventId: String,
    val orderId: String,
    val paymentId: String,
    val createdAtUnixMs: Long,
)

data class PaymentFailedEvent(
    val eventId: String,
    val orderId: String,
    val reason: String,
    val createdAtUnixMs: Long,
)

data class InventoryReleasedEvent(
    val eventId: String,
    val orderId: String,
    val reservationId: String,
    val reason: String,
    val createdAtUnixMs: Long,
)

data class OrderConfirmedEvent(
    val eventId: String,
    val orderId: String,
    val confirmedAtUnixMs: Long,
)

data class OrderFailedEvent(
    val eventId: String,
    val orderId: String,
    val reason: String,
    val failedAtUnixMs: Long,
)
