package systems.zlink.samples.kotlin.shoppingmall.server.commerceapi

import org.springframework.stereotype.Component
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToSpot
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleNames
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.ContinueOrderWorkflowReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.ContinueOrderWorkflowRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.OrderState
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.PrepareInventoryReservedReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.PrepareInventoryReservedRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.RebuildOrderProjectionReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.RebuildOrderProjectionRes
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.StartOrderWorkflowReq
import systems.zlink.samples.kotlin.shoppingmall.shared.contracts.StartOrderWorkflowRes

/**
 * Routes workflow commands from CommerceApi to the OrderWorkflow owner for an `OrderId`. Same
 * `OrderId` always reaches the same workflow instance channel.
 */
@Component
class OrderWorkflowRouter(private val routes: ZLinkRouteClient) {
    private val kotlinRoutes = routes.kotlin()

    suspend fun startWorkflow(command: StartOrderWorkflowReq): OrderState =
        request<StartOrderWorkflowRes>(command.orderId, command).state

    suspend fun prepareInventoryReserved(
        command: StartOrderWorkflowReq
    ): PrepareInventoryReservedRes =
        request<PrepareInventoryReservedRes>(command.orderId, PrepareInventoryReservedReq(command))

    suspend fun continueWorkflow(orderId: String): OrderState =
        request<ContinueOrderWorkflowRes>(orderId, ContinueOrderWorkflowReq(orderId)).state

    suspend fun rebuildProjection(orderId: String): OrderState =
        request<RebuildOrderProjectionRes>(orderId, RebuildOrderProjectionReq(orderId)).state

    // --8<-- [start:doc-sm-api-request]
    private suspend inline fun <reified TReply : Any> request(
        orderId: String,
        payload: Any,
    ): TReply {
        return kotlinRoutes
            .requestToSpot<TReply>(orderId, payload)
            .instanceSpot(SampleNames.OrderWorkflowSpotType)
            .inMesh(SampleNames.OrderWorkflowMesh)
            .timeout(SampleTimings.WorkflowTimeout)
            .await()
    }
    // --8<-- [end:doc-sm-api-request]
}
