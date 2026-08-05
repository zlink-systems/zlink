package systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow

import com.fasterxml.jackson.databind.JsonNode
import java.util.UUID
import org.springframework.stereotype.Component
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.CommerceStore
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.CommerceStore.StoredEvent
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.InventoryReservationFailedEvent
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.InventoryReleasedEvent
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.InventoryReservedEvent
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.InventoryReservationResult
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.OrderAggregate
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.OrderEventTypes
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.OrderLine
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.OrderProjection
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.OrderStartedEvent
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.OrderStatus
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.OrderConfirmedEvent
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.OrderFailedEvent
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.PaymentAuthorizedEvent
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.PaymentAuthorizationResult
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.PaymentFailedEvent
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.StartOrderCommand
import systems.zlink.samples.kotlin.shoppingmall.server.orderworkflow.domain.StoredOrderEvent
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.OrderLineInput
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.OrderState
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.StartOrderWorkflowReq

/**
 * Application service owning the event-sourced order workflow: loads the event
 * stream, rebuilds the aggregate, appends events with optimistic version
 * checks, advances the saga, and refreshes the projection.
 */
@Component
class OrderWorkflowService(private val store: CommerceStore) {
    private val json = store.json

    fun start(command: StartOrderWorkflowReq): OrderState {
        val stored = store.readEvents(command.orderId)
        val aggregate = OrderAggregate.rehydrate(command.orderId, stored.toDomainEvents())
        if (aggregate.hasProcessedCommand(command.idempotencyKey)) {
            return requireProjection(command.orderId)
        }
        val events = aggregate.start(toDomainCommand(command), newEventId("started", command.orderId), now())
        if (events.isNotEmpty()) {
            appendAndProject(command.orderId, stored.size.toLong(), events)
        }
        store.markIdempotencyStarted(command.idempotencyKey)
        return requireProjection(command.orderId)
    }

    fun prepareInventoryReserved(command: StartOrderWorkflowReq): OrderState {
        val started = store.readEvents(command.orderId)
        val aggregate = OrderAggregate.rehydrate(command.orderId, started.toDomainEvents())
        val startEvents = aggregate.start(toDomainCommand(command), newEventId("started", command.orderId), now())
        if (startEvents.isNotEmpty()) {
            appendAndProject(command.orderId, started.size.toLong(), startEvents)
            store.markIdempotencyStarted(command.idempotencyKey)
        }

        val afterStart = store.readEvents(command.orderId)
        val current = OrderAggregate.rehydrate(command.orderId, afterStart.toDomainEvents())
        if (current.status() == OrderStatus.Created) {
            val reserve = store.reserveInventory(command.orderId, current.lines().toContractLines())
            val reserveEvents = current.applyInventoryResult(
                InventoryReservationResult(reserve.accepted, reserve.reservationId, reserve.reason),
                newEventId("reserved", command.orderId),
                newEventId("inv-fail", command.orderId),
                now(),
            )
            if (reserveEvents.isNotEmpty()) {
                appendAndProject(command.orderId, afterStart.size.toLong(), reserveEvents)
            }
        }
        return requireProjection(command.orderId)
    }

    fun continueWorkflow(orderId: String): OrderState {
        // Each iteration advances one event-stream transition; a started order
        // reaches a terminal state within a handful of steps.
        for (step in 0 until MAX_WORKFLOW_STEPS) {
            val stored = store.readEvents(orderId)
            val aggregate = OrderAggregate.rehydrate(orderId, stored.toDomainEvents())
            if (aggregate.isTerminal() || !aggregate.hasStarted()) {
                return store.findReadModel(orderId) ?: store.placeholder(orderId)
            }
            val status = aggregate.status()
            val ts = now()
            val next: List<Any> = when (status) {
                OrderStatus.Created -> {
                    val reserve = store.reserveInventory(orderId, aggregate.lines().toContractLines())
                    aggregate.applyInventoryResult(
                        InventoryReservationResult(reserve.accepted, reserve.reservationId, reserve.reason),
                        newEventId("reserved", orderId),
                        newEventId("inv-fail", orderId),
                        ts,
                    )
                }
                OrderStatus.InventoryReserved -> {
                    val payment = store.authorizePayment(
                        orderId,
                        aggregate.paymentMethodId(),
                        aggregate.amount(),
                        aggregate.currency(),
                    )
                    aggregate.applyPaymentResult(
                        PaymentAuthorizationResult(payment.accepted, payment.paymentId, payment.reason),
                        newEventId("paid", orderId),
                        newEventId("released", orderId),
                        newEventId("pay-fail", orderId),
                        ts,
                    )
                }
                OrderStatus.PaymentAuthorized -> aggregate.confirm(newEventId("confirmed", orderId), ts)
                else -> emptyList()
            }
            if (next.isEmpty()) {
                return requireProjection(orderId)
            }
            appendAndProject(orderId, stored.size.toLong(), next)
            for (event in next) {
                if (event is InventoryReleasedEvent) {
                    store.releaseInventory(orderId, event.reservationId, event.reason)
                }
            }
        }
        return store.findReadModel(orderId) ?: store.placeholder(orderId)
    }

    fun rebuildProjection(orderId: String): OrderState {
        val stored = store.readEvents(orderId)
        if (stored.isEmpty()) {
            throw IllegalStateException("No order event stream for '$orderId'.")
        }
        var state: OrderProjection.State? = null
        for (event in stored.toDomainEvents()) {
            state = OrderProjection.apply(state, event)
        }
        val readModel = state!!.toReadModel()
        store.saveReadModel(readModel)
        return readModel
    }

    private fun appendAndProject(orderId: String, expectedVersion: Long, events: List<Any>) {
        val toStore = events.map { toStored(orderId, it) }
        store.appendEvents(orderId, expectedVersion, toStore)
        var state: OrderProjection.State? = null
        for (stored in store.readEvents(orderId).toDomainEvents()) {
            state = OrderProjection.apply(state, stored)
        }
        store.saveReadModel(state!!.toReadModel())
    }

    private fun toStored(orderId: String, event: Any): StoredEvent {
        val payload = json.valueToTree<JsonNode>(event)
        val eventType = event.javaClass.simpleName
        val eventId = payload.path("eventId").asText(newEventId("evt", orderId))
        val sourceCommandId = if (event is OrderStartedEvent) event.sourceCommandId else null
        val createdAt = payload.path("createdAtUnixMs").asLong(
            payload.path("confirmedAtUnixMs").asLong(
                payload.path("failedAtUnixMs").asLong(now()),
            ),
        )
        return StoredEvent(eventId, sourceCommandId, orderId, eventType, payload, 0, createdAt)
    }

    private fun List<StoredEvent>.toDomainEvents(): List<StoredOrderEvent> =
        map { it.toDomainEvent() }

    private fun StoredEvent.toDomainEvent(): StoredOrderEvent =
        StoredOrderEvent(
            eventId,
            sourceCommandId,
            orderId,
            eventType,
            when (eventType) {
                OrderEventTypes.OrderStarted -> json.convertValue(payload, OrderStartedEvent::class.java)
                OrderEventTypes.InventoryReserved -> json.convertValue(payload, InventoryReservedEvent::class.java)
                OrderEventTypes.InventoryReservationFailed ->
                    json.convertValue(payload, InventoryReservationFailedEvent::class.java)
                OrderEventTypes.PaymentAuthorized -> json.convertValue(payload, PaymentAuthorizedEvent::class.java)
                OrderEventTypes.PaymentFailed -> json.convertValue(payload, PaymentFailedEvent::class.java)
                OrderEventTypes.InventoryReleased -> json.convertValue(payload, InventoryReleasedEvent::class.java)
                OrderEventTypes.OrderConfirmed -> json.convertValue(payload, OrderConfirmedEvent::class.java)
                OrderEventTypes.OrderFailed -> json.convertValue(payload, OrderFailedEvent::class.java)
                else -> throw IllegalStateException("Unknown order event type: $eventType")
            },
            version,
            createdAtUnixMs,
        )

    private fun toDomainCommand(command: StartOrderWorkflowReq): StartOrderCommand =
        StartOrderCommand(
            command.orderId,
            command.cartId,
            command.shippingAddressId,
            command.paymentMethodId,
            command.idempotencyKey,
            command.lines.map { OrderLine(it.sku, it.quantity) },
            command.amount,
            command.currency,
        )

    private fun List<OrderLine>.toContractLines(): List<OrderLineInput> =
        map { OrderLineInput(it.sku, it.quantity) }

    private fun OrderProjection.State.toReadModel(): OrderState =
        OrderState(
            orderId,
            status,
            shippingAddressId,
            reservationId,
            paymentId,
            reason,
            amount,
            currency,
            updatedAtUnixMs,
        )

    private fun requireProjection(orderId: String): OrderState =
        store.findReadModel(orderId)
            ?: throw IllegalStateException("Projection missing for '$orderId'.")

    private fun newEventId(prefix: String, orderId: String): String =
        "$prefix-$orderId-${UUID.randomUUID().toString().replace("-", "")}"

    private fun now(): Long = System.currentTimeMillis()

    private companion object {
        const val MAX_WORKFLOW_STEPS = 8
    }
}
