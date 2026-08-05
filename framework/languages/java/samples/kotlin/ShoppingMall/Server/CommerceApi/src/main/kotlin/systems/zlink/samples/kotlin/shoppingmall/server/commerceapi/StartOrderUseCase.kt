package systems.zlink.samples.kotlin.shoppingmall.server.commerceapi

import kotlinx.coroutines.delay
import kotlinx.coroutines.future.await
import org.springframework.stereotype.Component
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.CommerceStore
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleNames
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleTopology
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.OrderState
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.StartOrderReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.StartOrderRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.StartOrderWorkflowReq

/**
 * Order-start use case: idempotency lookup, cross-instance forwarding for
 * pending mappings owned elsewhere, input validation, and workflow relay.
 * Never appends domain events itself.
 */
@Component
class StartOrderUseCase(
    private val store: CommerceStore,
    private val workflows: OrderWorkflowRouter,
    private val channels: ZLinkClient,
    topology: SampleTopology,
) {
    private val instanceId = topology.role().instanceId

    suspend fun execute(request: StartOrderReq): StartOrderRes {
        val existing = store.findIdempotency(request.idempotencyKey)
        if (existing != null && existing.started) {
            val state = store.findReadModel(existing.orderId) ?: store.placeholder(existing.orderId)
            return StartOrderRes(state.orderId, state.status)
        }
        if (existing != null && existing.ownerInstanceId != instanceId) {
            return forwardToOwner(existing.ownerInstanceId, request)
        }

        val command = buildCommand(request, existing)
        val state = workflows.startWorkflow(command)
        return StartOrderRes(state.orderId, state.status)
    }

    suspend fun prepareInventoryReserved(request: StartOrderReq) =
        prepareInventoryReservedState(request)

    private suspend fun prepareInventoryReservedState(request: StartOrderReq): OrderState {
        val existing = store.findIdempotency(request.idempotencyKey)
        if (existing != null && existing.started) {
            return store.findReadModel(existing.orderId) ?: store.placeholder(existing.orderId)
        }
        if (existing != null && existing.ownerInstanceId != instanceId) {
            throw IllegalStateException("Inventory-reserved checkpoint must run on owning CommerceApi.")
        }

        val command = buildCommand(request, existing)
        return workflows.prepareInventoryReserved(command)
    }

    private fun buildCommand(
        request: StartOrderReq,
        existing: CommerceStore.IdempotencyMapping?,
    ): StartOrderWorkflowReq {
        val cart = store.findCart(request.cartId)
            ?: throw IllegalArgumentException("Unknown cart '${request.cartId}'.")
        if (!store.shippingAddressExists(request.shippingAddressId)) {
            throw IllegalArgumentException("Unknown shipping address '${request.shippingAddressId}'.")
        }
        if (!store.paymentMethodExists(request.paymentMethodId)) {
            throw IllegalArgumentException("Unknown payment method '${request.paymentMethodId}'.")
        }

        val mapping = existing
            ?: store.reserveIdempotency(request.idempotencyKey, instanceId)
        store.saveOrderPaymentMethod(mapping.orderId, request.paymentMethodId)

        return StartOrderWorkflowReq(
            mapping.orderId,
            request.cartId,
            request.shippingAddressId,
            request.paymentMethodId,
            request.idempotencyKey,
            cart.lines,
            cart.amount,
            cart.currency,
        )
    }

    private suspend fun forwardToOwner(ownerInstanceId: String, request: StartOrderReq): StartOrderRes {
        val channel = SampleNames.commerceApiChannel(ownerInstanceId)
        var lastError: RuntimeException? = null
        for (attempt in 1..SampleTimings.MaxChannelAttempts) {
            try {
                return channels.requestToChannel(channel, request)
                    .timeout(SampleTimings.RequestTimeout)
                    .submit(StartOrderRes::class.java)
                    .await()
            } catch (error: RuntimeException) {
                lastError = error
                delay(SampleTimings.ChannelRetryDelay.toMillis())
            }
        }
        throw IllegalStateException(
            "Peer CommerceApi '$ownerInstanceId' was not ready for forwarded start.", lastError,
        )
    }
}
