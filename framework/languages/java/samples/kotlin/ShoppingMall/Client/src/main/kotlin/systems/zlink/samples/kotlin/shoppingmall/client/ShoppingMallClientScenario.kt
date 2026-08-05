package systems.zlink.samples.kotlin.shoppingmall.client

import kotlinx.coroutines.delay
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.future.await
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.samples.kotlin.shoppingmall.client.configuration.SampleNames
import systems.zlink.samples.kotlin.shoppingmall.client.configuration.SampleTimings
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.ContinueOrderWorkflowReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.ContinueOrderWorkflowRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.CreatePendingMappingReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.CreatePendingMappingRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.DeleteProjectionReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.DeleteProjectionRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.GetOrderStateReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.GetOrderStateRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.OrderState
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.OrderStatuses
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.PrepareInventoryReservedApiReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.PrepareInventoryReservedApiRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.RebuildProjectionApiReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.RebuildProjectionApiRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.ServerAssertionReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.ServerAssertionRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.StartOrderReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.StartOrderRes

/**
 * Reads as a checkout scenario test: each request is followed immediately by
 * its response assertion. The client only talks to the two CommerceApi
 * instances; storage and workflow internals are verified server-side.
 */
class ShoppingMallClientScenario(
    private val channels: ZLinkClient,
) {
    private val apiA = SampleNames.ApiInstanceA
    private val apiB = SampleNames.ApiInstanceB

    suspend fun run() {
        val success = runSuccessfulOrder()
        runDuplicateIdempotency(success)
        val concurrentOrderId = runConcurrentIdempotency()
        val pendingOrderId = runPendingRecovery()
        val resumedOrderId = runExplicitResume()
        val inventoryOrderId = runInventoryFailure()
        val paymentOrderId = runPaymentFailure()
        runProjectionRebuild(success.orderId)
        runQueryConsistency(paymentOrderId)
        val scaleOrderId = runScaleOut()
        assertServerEvidence(
            success.orderId,
            pendingOrderId,
            concurrentOrderId,
            resumedOrderId,
            inventoryOrderId,
            paymentOrderId,
            scaleOrderId,
        )
    }

    private suspend fun runSuccessfulOrder(): StartOrderRes {
        val started = start(apiA, StartOrderReq("cart-success", "addr-home", "pm-ok", "order-success-001"))
        ensure(started.status == OrderStatuses.Created)
        ensure(started.orderId.isNotEmpty())

        val created = getState(apiA, started.orderId)
        ensure(created.isCreatedOrConfirmed())
        ensure(created.shippingAddressId == "addr-home")

        val confirmed = waitForStatus(apiA, started.orderId, OrderStatuses.Confirmed)
        ensure(confirmed.reservationId != null)
        ensure(confirmed.paymentId != null)
        ensure(confirmed.amount != null && Math.abs(confirmed.amount!! - 120.00) < 0.001)
        ensure(confirmed.currency == "USD")
        println("shoppingmall-success=completed")
        return started
    }

    private suspend fun runDuplicateIdempotency(success: StartOrderRes) {
        val duplicate = start(apiB, StartOrderReq("cart-success", "addr-home", "pm-ok", "order-success-001"))
        ensure(duplicate.orderId == success.orderId)
        println("shoppingmall-idempotency=completed")
    }

    private suspend fun runConcurrentIdempotency(): String = coroutineScope {
        val request = StartOrderReq("cart-success", "addr-office", "pm-ok", "order-concurrent-001")
        val startedA = async { start(apiA, request) }
        val startedB = async { start(apiB, request) }
        val resultA = startedA.await()
        val resultB = startedB.await()
        ensure(resultA.orderId == resultB.orderId)
        val confirmed = waitForStatus(apiA, resultA.orderId, OrderStatuses.Confirmed)
        ensure(confirmed.status == OrderStatuses.Confirmed)
        println("shoppingmall-concurrent=completed")
        resultA.orderId
    }

    private suspend fun runPendingRecovery(): String {
        val pending = channels
            .requestToChannel(
                SampleNames.commerceApiChannel(apiA),
                CreatePendingMappingReq("order-pending-001", "order-pending-0001", apiA),
            )
            .timeout(SampleTimings.RequestTimeout)
            .submit(CreatePendingMappingRes::class.java)
            .await()
        ensure(pending.created)

        val started = start(apiB, StartOrderReq("cart-success", "addr-office", "pm-ok", "order-pending-001"))
        ensure(started.orderId == "order-pending-0001")
        ensure(started.status == OrderStatuses.Created)

        val created = waitForCreatedOrConfirmed(apiA, started.orderId)
        ensure(created.isCreatedOrConfirmed())
        ensure(created.shippingAddressId == "addr-office")
        println("shoppingmall-pending=completed")
        return started.orderId
    }

    private suspend fun runExplicitResume(): String {
        val prepared = channels
            .requestToChannel(
                SampleNames.commerceApiChannel(apiA),
                PrepareInventoryReservedApiReq(
                    StartOrderReq("cart-success", "addr-home", "pm-ok", "order-resume-001"),
                ),
            )
            .timeout(SampleTimings.WorkflowTimeout)
            .submit(PrepareInventoryReservedApiRes::class.java)
            .await()
        ensure(prepared.state.status == OrderStatuses.InventoryReserved)

        val resumed = channels
            .requestToChannel(
                SampleNames.commerceApiChannel(apiB),
                ContinueOrderWorkflowReq(prepared.state.orderId),
            )
            .timeout(SampleTimings.WorkflowTimeout)
            .submit(ContinueOrderWorkflowRes::class.java)
            .await()
        ensure(resumed.state.status == OrderStatuses.Confirmed)
        ensure(resumed.state.reservationId == "reservation-${prepared.state.orderId}")
        ensure(resumed.state.paymentId == "payment-${prepared.state.orderId}")
        println("shoppingmall-resume=completed")
        return prepared.state.orderId
    }

    private suspend fun runInventoryFailure(): String {
        val started = start(apiA, StartOrderReq("cart-inventory-fail", "addr-home", "pm-ok", "order-inventory-001"))
        val failed = waitForStatus(apiA, started.orderId, OrderStatuses.Failed)
        ensure(failed.reason != null && failed.reason!!.lowercase().contains("inventory"))
        val reread = getState(apiA, started.orderId)
        ensure(reread.status == OrderStatuses.Failed)
        println("shoppingmall-inventory-failure=completed")
        return started.orderId
    }

    private suspend fun runPaymentFailure(): String {
        val started = start(apiB, StartOrderReq("cart-payment-fail", "addr-home", "pm-decline", "order-payment-001"))
        val failed = waitForStatus(apiB, started.orderId, OrderStatuses.Failed)
        ensure(failed.reservationId != null)
        ensure(failed.reason != null && failed.reason!!.lowercase().contains("payment"))
        println("shoppingmall-payment-failure=completed")
        return started.orderId
    }

    private suspend fun runProjectionRebuild(orderId: String) {
        val deleted = channels
            .requestToChannel(SampleNames.commerceApiChannel(apiA), DeleteProjectionReq(orderId))
            .timeout(SampleTimings.RequestTimeout)
            .submit(DeleteProjectionRes::class.java)
            .await()
        ensure(deleted.deleted)

        val rebuilt = channels
            .requestToChannel(SampleNames.commerceApiChannel(apiA), RebuildProjectionApiReq(orderId))
            .timeout(SampleTimings.WorkflowTimeout)
            .submit(RebuildProjectionApiRes::class.java)
            .await()
        ensure(rebuilt.state.status == OrderStatuses.Confirmed)

        val fromB = getState(apiB, orderId)
        ensure(fromB.status == OrderStatuses.Confirmed)
        println("shoppingmall-rebuild=completed")
    }

    private suspend fun runQueryConsistency(orderId: String) {
        val fromB = getState(apiB, orderId)
        val fromA = getState(apiA, orderId)
        ensure(fromA.status == fromB.status)
        ensure(fromA.status == OrderStatuses.Failed)
        println("shoppingmall-consistency=completed")
    }

    private suspend fun runScaleOut(): String {
        val started = start(apiB, StartOrderReq("cart-success", "addr-office", "pm-ok", "order-scale-001"))
        val confirmed = waitForStatus(apiA, started.orderId, OrderStatuses.Confirmed)
        ensure(confirmed.status == OrderStatuses.Confirmed)
        println("shoppingmall-scaleout=completed")
        return started.orderId
    }

    private suspend fun assertServerEvidence(
        successOrderId: String,
        pendingOrderId: String,
        concurrentOrderId: String,
        resumedOrderId: String,
        inventoryOrderId: String,
        paymentOrderId: String,
        scaleOrderId: String,
    ) {
        val assertion = channels
            .requestToChannel(
                SampleNames.commerceApiChannel(apiA),
                ServerAssertionReq(
                    successOrderId,
                    pendingOrderId,
                    concurrentOrderId,
                    resumedOrderId,
                    inventoryOrderId,
                    paymentOrderId,
                    scaleOrderId,
                ),
            )
            .timeout(SampleTimings.RequestTimeout)
            .submit(ServerAssertionRes::class.java)
            .await()
        ensure(assertion.passed)
        println("shoppingmall-server-evidence=completed")
    }

    private suspend fun start(instanceId: String, request: StartOrderReq): StartOrderRes =
        channels
            .requestToChannel(SampleNames.commerceApiChannel(instanceId), request)
            .timeout(SampleTimings.RequestTimeout)
            .submit(StartOrderRes::class.java)
            .await()

    private suspend fun getState(instanceId: String, orderId: String): OrderState =
        channels
            .requestToChannel(SampleNames.commerceApiChannel(instanceId), GetOrderStateReq(orderId))
            .timeout(SampleTimings.RequestTimeout)
            .submit(GetOrderStateRes::class.java)
            .await()
            .state

    private suspend fun waitForStatus(instanceId: String, orderId: String, expectedStatus: String): OrderState {
        val deadline = System.nanoTime() + SampleTimings.WorkflowTimeout.toNanos()
        var last: OrderState? = null
        while (System.nanoTime() < deadline) {
            last = getState(instanceId, orderId)
            if (last.status == expectedStatus) {
                return last
            }
            delay(SampleTimings.PollDelay.toMillis())
        }
        throw IllegalStateException(
            "Order '$orderId' did not reach status '$expectedStatus' (last=${last?.status ?: "none"}).",
        )
    }

    private suspend fun waitForCreatedOrConfirmed(instanceId: String, orderId: String): OrderState {
        val deadline = System.nanoTime() + SampleTimings.WorkflowTimeout.toNanos()
        var last: OrderState? = null
        while (System.nanoTime() < deadline) {
            last = getState(instanceId, orderId)
            if (last.isCreatedOrConfirmed()) {
                return last
            }
            delay(SampleTimings.PollDelay.toMillis())
        }
        throw IllegalStateException(
            "Order '$orderId' did not reach Created or Confirmed (last=${last?.status ?: "none"}).",
        )
    }

    private fun ensure(condition: Boolean) {
        if (!condition) {
            throw IllegalStateException("Ensure failed")
        }
    }

    private fun OrderState.isCreatedOrConfirmed(): Boolean =
        status == OrderStatuses.Created || status == OrderStatuses.Confirmed
}
