package systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain

/**
 * Order aggregate rebuilt from the event stream. Owns the status state machine,
 * command dedupe, and compensation rules. Knows nothing about framework,
 * transport, or store implementation.
 */
class OrderAggregate private constructor(private val orderId: String) {
    private val processedCommands = HashSet<String>()
    private var status: String? = null
    private var reservationId: String? = null
    private var paymentMethodId: String? = null
    private var amount: Double = 0.0
    private var currency: String = "USD"
    private var lines: List<OrderLine> = emptyList()

    companion object {
        fun rehydrate(
            orderId: String,
            stored: List<StoredOrderEvent>,
        ): OrderAggregate {
            val aggregate = OrderAggregate(orderId)
            for (event in stored) {
                aggregate.apply(event)
            }
            return aggregate
        }
    }

    private fun apply(event: StoredOrderEvent) {
        when (event.eventType) {
            OrderEventTypes.OrderStarted -> {
                val started = event.payload as OrderStartedEvent
                status = OrderStatus.Created
                paymentMethodId = started.paymentMethodId
                amount = started.amount
                currency = started.currency
                lines = started.lines
                processedCommands.add(started.sourceCommandId)
            }
            OrderEventTypes.InventoryReserved -> {
                val reserved = event.payload as InventoryReservedEvent
                status = OrderStatus.InventoryReserved
                reservationId = reserved.reservationId
            }
            OrderEventTypes.InventoryReservationFailed -> status = OrderStatus.Failed
            OrderEventTypes.PaymentAuthorized -> status = OrderStatus.PaymentAuthorized
            OrderEventTypes.PaymentFailed -> status = OrderStatus.Failed
            OrderEventTypes.OrderConfirmed -> status = OrderStatus.Confirmed
            OrderEventTypes.OrderFailed -> status = OrderStatus.Failed
        }
    }

    fun hasStarted(): Boolean = status != null

    fun isTerminal(): Boolean =
        status == OrderStatus.Confirmed || status == OrderStatus.Failed

    fun status(): String? = status

    fun lines(): List<OrderLine> = lines

    fun paymentMethodId(): String = paymentMethodId ?: ""

    fun amount(): Double = amount

    fun currency(): String = currency

    fun hasProcessedCommand(sourceCommandId: String): Boolean =
        processedCommands.contains(sourceCommandId)

    fun start(command: StartOrderCommand, eventId: String, now: Long): List<Any> {
        if (hasStarted()) {
            return emptyList()
        }
        return listOf(
            OrderStartedEvent(
                eventId,
                command.idempotencyKey,
                orderId,
                command.cartId,
                command.shippingAddressId,
                command.paymentMethodId,
                command.lines,
                command.amount,
                command.currency,
                now,
            ),
        )
    }

    fun applyInventoryResult(
        result: InventoryReservationResult,
        reservedEventId: String,
        failedEventId: String,
        now: Long,
    ): List<Any> {
        if (isTerminal() || status != OrderStatus.Created) {
            return emptyList()
        }
        if (!result.accepted) {
            val reason = result.reason ?: "inventory unavailable"
            return listOf(
                InventoryReservationFailedEvent(failedEventId, orderId, reason, now),
                OrderFailedEvent("$failedEventId-failed", orderId, reason, now),
            )
        }
        return listOf(InventoryReservedEvent(reservedEventId, orderId, result.reservationId!!, now))
    }

    fun applyPaymentResult(
        result: PaymentAuthorizationResult,
        paymentEventId: String,
        releaseEventId: String,
        failedEventId: String,
        now: Long,
    ): List<Any> {
        if (isTerminal() || status != OrderStatus.InventoryReserved) {
            return emptyList()
        }
        if (!result.accepted) {
            val reason = result.reason ?: "payment failed"
            return listOf(
                PaymentFailedEvent(paymentEventId, orderId, reason, now),
                InventoryReleasedEvent(releaseEventId, orderId, reservationId!!, reason, now),
                OrderFailedEvent(failedEventId, orderId, reason, now),
            )
        }
        return listOf(PaymentAuthorizedEvent(paymentEventId, orderId, result.paymentId!!, now))
    }

    fun confirm(eventId: String, now: Long): List<Any> {
        if (isTerminal() || status != OrderStatus.PaymentAuthorized) {
            return emptyList()
        }
        return listOf(OrderConfirmedEvent(eventId, orderId, now))
    }
}

data class StoredOrderEvent(
    val eventId: String,
    val sourceCommandId: String?,
    val orderId: String,
    val eventType: String,
    val payload: Any,
    val version: Long,
    val createdAtUnixMs: Long,
)

data class InventoryReservationResult(
    val accepted: Boolean,
    val reservationId: String?,
    val reason: String?,
)

data class PaymentAuthorizationResult(
    val accepted: Boolean,
    val paymentId: String?,
    val reason: String?,
)
