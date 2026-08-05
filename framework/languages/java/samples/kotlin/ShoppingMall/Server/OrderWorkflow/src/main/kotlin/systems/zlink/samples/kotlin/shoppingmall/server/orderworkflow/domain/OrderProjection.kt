package systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain

/**
 * Pure projection folder: applies a stored event to the running order state.
 * The projection is rebuildable from the event stream alone.
 */
object OrderProjection {
    fun apply(current: State?, event: StoredOrderEvent): State {
        val ts = event.createdAtUnixMs
        return when (event.eventType) {
            OrderEventTypes.OrderStarted -> {
                val started = event.payload as OrderStartedEvent
                State(
                started.orderId,
                OrderStatus.Created,
                started.shippingAddressId,
                null,
                null,
                null,
                started.amount,
                started.currency,
                ts,
            )
            }
            OrderEventTypes.InventoryReserved -> require(current).copy(
                status = OrderStatus.InventoryReserved,
                reservationId = (event.payload as InventoryReservedEvent).reservationId,
                updatedAtUnixMs = ts,
            )
            OrderEventTypes.InventoryReservationFailed -> require(current).copy(
                status = OrderStatus.Failed,
                reason = (event.payload as InventoryReservationFailedEvent).reason,
                updatedAtUnixMs = ts,
            )
            OrderEventTypes.PaymentAuthorized -> require(current).copy(
                status = OrderStatus.PaymentAuthorized,
                paymentId = (event.payload as PaymentAuthorizedEvent).paymentId,
                updatedAtUnixMs = ts,
            )
            OrderEventTypes.PaymentFailed -> require(current).copy(
                status = OrderStatus.Failed,
                reason = (event.payload as PaymentFailedEvent).reason,
                updatedAtUnixMs = ts,
            )
            OrderEventTypes.InventoryReleased -> require(current).copy(
                reason = (event.payload as InventoryReleasedEvent).reason,
                updatedAtUnixMs = ts,
            )
            OrderEventTypes.OrderConfirmed -> require(current).copy(
                status = OrderStatus.Confirmed,
                updatedAtUnixMs = ts,
            )
            OrderEventTypes.OrderFailed -> require(current).copy(
                status = OrderStatus.Failed,
                reason = (event.payload as OrderFailedEvent).reason,
                updatedAtUnixMs = ts,
            )
            else -> require(current)
        }
    }

    private fun require(current: State?): State =
        current ?: throw IllegalStateException("Cannot apply order event before OrderStartedEvent.")

    data class State(
        val orderId: String,
        val status: String,
        val shippingAddressId: String?,
        val reservationId: String?,
        val paymentId: String?,
        val reason: String?,
        val amount: Double?,
        val currency: String?,
        val updatedAtUnixMs: Long,
    )
}
