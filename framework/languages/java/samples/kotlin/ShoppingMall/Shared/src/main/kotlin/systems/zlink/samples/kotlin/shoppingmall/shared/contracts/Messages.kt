package systems.zlink.samples.kotlin.shoppingmall.shared.contracts

import systems.zlink.framework.handlers.ZLinkPacket

object OrderStatuses {
    const val Created = "Created"
    const val InventoryReserved = "InventoryReserved"
    const val PaymentAuthorized = "PaymentAuthorized"
    const val Confirmed = "Confirmed"
    const val Failed = "Failed"
}

data class OrderLineInput(val sku: String, val quantity: Int)

data class OrderState(
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

// ----- client HTTP-style contracts -----

@ZLinkPacket("StartOrderReq")
data class StartOrderReq(
    val cartId: String,
    val shippingAddressId: String,
    val paymentMethodId: String,
    val idempotencyKey: String,
)

data class StartOrderRes(val orderId: String, val status: String)

@ZLinkPacket("GetOrderStateReq")
data class GetOrderStateReq(val orderId: String)

data class GetOrderStateRes(val state: OrderState)

@ZLinkPacket("DeleteProjectionReq")
data class DeleteProjectionReq(val orderId: String)

data class DeleteProjectionRes(val deleted: Boolean)

@ZLinkPacket("RebuildProjectionApiReq")
data class RebuildProjectionApiReq(val orderId: String)

data class RebuildProjectionApiRes(val state: OrderState)

@ZLinkPacket("PrepareInventoryReservedApiReq")
data class PrepareInventoryReservedApiReq(val request: StartOrderReq)

data class PrepareInventoryReservedApiRes(val state: OrderState)

@ZLinkPacket("PrepareInventoryReservedReq")
data class PrepareInventoryReservedReq(val command: StartOrderWorkflowReq)

data class PrepareInventoryReservedRes(val state: OrderState)

@ZLinkPacket("CreatePendingMappingReq")
data class CreatePendingMappingReq(
    val idempotencyKey: String,
    val orderId: String,
    val ownerInstanceId: String,
)

data class CreatePendingMappingRes(val created: Boolean)

@ZLinkPacket("ServerAssertionReq")
data class ServerAssertionReq(
    val successfulOrderId: String,
    val pendingRecoveredOrderId: String,
    val concurrentOrderId: String,
    val resumedOrderId: String,
    val inventoryFailureOrderId: String,
    val paymentFailureOrderId: String,
    val scaleOutOrderId: String,
)

data class ServerAssertionRes(val passed: Boolean, val evidence: List<String>)

// ----- workflow command contracts (CommerceApi -> OrderWorkflow) -----

@ZLinkPacket("StartOrderWorkflowReq")
data class StartOrderWorkflowReq(
    val orderId: String,
    val cartId: String,
    val shippingAddressId: String,
    val paymentMethodId: String,
    val idempotencyKey: String,
    val lines: List<OrderLineInput>,
    val amount: Double,
    val currency: String,
)

data class StartOrderWorkflowRes(val state: OrderState)

@ZLinkPacket("ContinueOrderWorkflowReq")
data class ContinueOrderWorkflowReq(val orderId: String)

data class ContinueOrderWorkflowRes(val state: OrderState)

@ZLinkPacket("RebuildOrderProjectionReq")
data class RebuildOrderProjectionReq(val orderId: String)

data class RebuildOrderProjectionRes(val state: OrderState)

// ----- self-check seed contracts -----

data class CartSeed(
    val cartId: String,
    val lines: List<OrderLineInput>,
    val amount: Double,
    val currency: String,
)

data class InventorySeed(val sku: String, val availableQuantity: Int)

data class PaymentMethodSeed(
    val paymentMethodId: String,
    val shouldAuthorize: Boolean,
    val failureReason: String?,
)
