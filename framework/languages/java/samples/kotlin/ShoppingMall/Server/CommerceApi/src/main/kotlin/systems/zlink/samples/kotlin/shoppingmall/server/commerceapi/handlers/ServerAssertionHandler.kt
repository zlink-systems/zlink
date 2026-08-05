package systems.zlink.samples.kotlin.shoppingmall.server.commerceapi.handlers

import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.CommerceStore
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.CommerceStore.StoreEvidence
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.ServerAssertionReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.ServerAssertionRes

/**
 * Server-side assertion of the event-sourced evidence: per-order event
 * sequences, compensation counts, and idempotency started count.
 */
@ZLinkHandlerGroup("commerce")
class ServerAssertionHandler(
    private val store: CommerceStore,
) : ZLinkSuspendingRequestHandler<ServerAssertionReq, ServerAssertionRes> {
    override suspend fun handle(
        request: ServerAssertionReq,
        context: ZLinkMessageContext,
    ): ServerAssertionRes {
        val orderIds = listOf(
            request.successfulOrderId,
            request.pendingRecoveredOrderId,
            request.concurrentOrderId,
            request.resumedOrderId,
            request.inventoryFailureOrderId,
            request.paymentFailureOrderId,
            request.scaleOutOrderId,
        )
        val evidence = store.evidence(orderIds)
        val lines = mutableListOf<String>()
        var passed = true

        passed = check(lines, evidence, request.successfulOrderId, listOf(
            "OrderStartedEvent", "InventoryReservedEvent",
            "PaymentAuthorizedEvent", "OrderConfirmedEvent",
        )) && passed
        passed = startsWith(lines, evidence, request.pendingRecoveredOrderId, listOf(
            "OrderStartedEvent",
        )) && passed
        passed = check(lines, evidence, request.concurrentOrderId, listOf(
            "OrderStartedEvent", "InventoryReservedEvent",
            "PaymentAuthorizedEvent", "OrderConfirmedEvent",
        )) && passed
        passed = check(lines, evidence, request.resumedOrderId, listOf(
            "OrderStartedEvent", "InventoryReservedEvent",
            "PaymentAuthorizedEvent", "OrderConfirmedEvent",
        )) && passed
        passed = check(lines, evidence, request.inventoryFailureOrderId, listOf(
            "OrderStartedEvent", "InventoryReservationFailedEvent", "OrderFailedEvent",
        )) && passed
        passed = check(lines, evidence, request.paymentFailureOrderId, listOf(
            "OrderStartedEvent", "InventoryReservedEvent", "PaymentFailedEvent",
            "InventoryReleasedEvent", "OrderFailedEvent",
        )) && passed
        passed = check(lines, evidence, request.scaleOutOrderId, listOf(
            "OrderStartedEvent", "InventoryReservedEvent",
            "PaymentAuthorizedEvent", "OrderConfirmedEvent",
        )) && passed

        val compensation = evidence.releasedReservationCount >= 1 && evidence.paymentFailureCount >= 1
        val startedCount = evidence.startedIdempotencyCount == 7
        lines.add("releasedReservationCount=${evidence.releasedReservationCount}")
        lines.add("paymentFailureCount=${evidence.paymentFailureCount}")
        lines.add("startedIdempotencyCount=${evidence.startedIdempotencyCount}")
        passed = compensation && startedCount && passed

        System.err.println("shoppingmall evidence: passed=$passed")
        return ServerAssertionRes(passed, lines)
    }

    private fun check(
        lines: MutableList<String>,
        evidence: StoreEvidence,
        orderId: String,
        expected: List<String>,
    ): Boolean {
        val actual = evidence.eventsByOrder[orderId] ?: emptyList()
        val match = actual == expected
        lines.add("$orderId=$actual" + if (match) " (ok)" else " (expected $expected)")
        return match
    }

    private fun startsWith(
        lines: MutableList<String>,
        evidence: StoreEvidence,
        orderId: String,
        prefix: List<String>,
    ): Boolean {
        val actual = evidence.eventsByOrder[orderId] ?: emptyList()
        val match = actual.size >= prefix.size && actual.subList(0, prefix.size) == prefix
        lines.add("$orderId=$actual" + if (match) " (ok prefix)" else " (expected prefix $prefix)")
        return match
    }
}
