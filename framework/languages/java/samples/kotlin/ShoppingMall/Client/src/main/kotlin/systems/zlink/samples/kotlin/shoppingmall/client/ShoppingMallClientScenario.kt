package systems.zlink.samples.kotlin.shoppingmall.client

import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import systems.zlink.httpclient.ZLinkHttpClient
import systems.zlink.httpclient.kotlin.fetch
import systems.zlink.samples.kotlin.shoppingmall.client.configuration.SampleTimings
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.GetOrderStateRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.ContinueOrderWorkflowRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.OrderState
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.OrderStatuses
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.RebuildProjectionApiRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.StartOrderReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.StartOrderRes

/** Exercises only the public CommerceApi HTTP order surface. */
class ShoppingMallClientScenario(
    private val apiA: String,
    private val apiB: String,
    private val pendingIdempotencyKey: String,
    private val pendingOrderId: String,
    private val resumeOrderId: String,
    private val rebuildOrderId: String,
) {
    suspend fun run() {
        val success = start(apiA, StartOrderReq("cart-success", "addr-home", "pm-ok", "order-success-001"))
        ensure(success.status == OrderStatuses.Created)
        val confirmed = waitForStatus(apiA, success.orderId, OrderStatuses.Confirmed)
        ensure(confirmed.reservationId != null && confirmed.paymentId != null)
        val amount = confirmed.amount
        ensure(amount != null && kotlin.math.abs(amount - 120.00) < 0.001)
        ensure(confirmed.currency == "USD")
        emitOrder("success", success)

        val duplicate = start(apiB, StartOrderReq("cart-success", "addr-home", "pm-ok", "order-success-001"))
        ensure(duplicate.orderId == success.orderId)

        val concurrent = runConcurrentIdempotency()
        emitOrder("concurrent", concurrent)

        val pending = start(apiB, StartOrderReq("cart-success", "addr-office", "pm-ok", pendingIdempotencyKey))
        ensure(pending.orderId == pendingOrderId)
        ensure(waitForStatus(apiA, pending.orderId, OrderStatuses.Confirmed).status == OrderStatuses.Confirmed)
        emitOrder("pending", pending)

        val resumed = post<ContinueOrderWorkflowRes>(apiB, "/orders/$resumeOrderId/continue", "")
        ensure(resumed.state.status == OrderStatuses.Confirmed)
        emitOrder("resumed", StartOrderRes(resumeOrderId, resumed.state.status))

        val rebuilt = post<RebuildProjectionApiRes>(apiA, "/orders/$rebuildOrderId/rebuild", "")
        ensure(rebuilt.state.status == OrderStatuses.Confirmed)
        ensure(getState(apiB, rebuildOrderId).status == OrderStatuses.Confirmed)
        emitOrder("rebuild", StartOrderRes(rebuildOrderId, rebuilt.state.status))

        val inventoryFailure = start(
            apiA,
            StartOrderReq("cart-inventory-fail", "addr-home", "pm-ok", "order-inventory-001"),
        )
        ensure(waitForStatus(apiA, inventoryFailure.orderId, OrderStatuses.Failed).reason?.lowercase()?.contains("inventory") == true)
        emitOrder("inventory-failure", inventoryFailure)

        val paymentFailure = start(
            apiB,
            StartOrderReq("cart-payment-fail", "addr-home", "pm-decline", "order-payment-001"),
        )
        ensure(waitForStatus(apiB, paymentFailure.orderId, OrderStatuses.Failed).reason?.lowercase()?.contains("payment") == true)
        emitOrder("payment-failure", paymentFailure)

        val scaleOut = start(apiB, StartOrderReq("cart-success", "addr-office", "pm-ok", "order-scale-001"))
        ensure(waitForStatus(apiA, scaleOut.orderId, OrderStatuses.Confirmed).status == OrderStatuses.Confirmed)
        emitOrder("scale-out", scaleOut)
    }

    private suspend fun runConcurrentIdempotency(): StartOrderRes = coroutineScope {
        val request = StartOrderReq("cart-success", "addr-office", "pm-ok", "order-concurrent-001")
        val first = async { start(apiA, request) }
        val second = async { start(apiB, request) }
        val resultA = first.await()
        val resultB = second.await()
        ensure(resultA.orderId == resultB.orderId)
        ensure(waitForStatus(apiA, resultA.orderId, OrderStatuses.Confirmed).status == OrderStatuses.Confirmed)
        resultA
    }

    private suspend fun start(base: String, request: StartOrderReq): StartOrderRes =
        post(base, "/orders/start", request)

    private suspend fun getState(base: String, orderId: String): OrderState =
        ZLinkHttpClient.create(base).get("/orders/$orderId").fetch<GetOrderStateRes>().state

    private suspend fun waitForStatus(base: String, orderId: String, expected: String): OrderState {
        repeat((SampleTimings.WorkflowTimeout.toMillis() / SampleTimings.PollDelay.toMillis()).toInt()) {
            val state = getState(base, orderId)
            if (state.status == expected) return state
            delay(SampleTimings.PollDelay.toMillis())
        }
        error("Order '$orderId' did not reach status '$expected'.")
    }

    private suspend inline fun <reified T> post(base: String, path: String, body: Any): T {
        val request = ZLinkHttpClient.create(base).post(path)
        if (body !is String || body.isNotEmpty()) request.body(body)
        return request.fetch()
    }

    private fun emitOrder(name: String, order: StartOrderRes) {
        println("shoppingmall-client-order name=$name order=${order.orderId}")
    }

    private fun ensure(condition: Boolean) {
        check(condition) { "Ensure failed" }
    }
}
