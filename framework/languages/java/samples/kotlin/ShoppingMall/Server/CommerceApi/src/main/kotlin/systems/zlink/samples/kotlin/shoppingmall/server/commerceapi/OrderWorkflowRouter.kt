package systems.zlink.samples.kotlin.shoppingmall.server.commerceapi

import kotlinx.coroutines.future.await
import org.springframework.stereotype.Component
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleNames
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.shoppingmall.server.configuration.SampleTopology
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
 * Routes workflow commands from CommerceApi to the OrderWorkflow owner for an
 * `OrderId`. Same `OrderId` always reaches the same workflow instance channel.
 */
@Component
class OrderWorkflowRouter(private val routes: ZLinkRouteClient) {
    suspend fun startWorkflow(command: StartOrderWorkflowReq): OrderState =
        request(command.orderId, command, StartOrderWorkflowRes::class.java).state

    suspend fun prepareInventoryReserved(command: StartOrderWorkflowReq): OrderState =
        request(command.orderId, PrepareInventoryReservedReq(command), PrepareInventoryReservedRes::class.java).state

    suspend fun continueWorkflow(orderId: String): OrderState =
        request(orderId, ContinueOrderWorkflowReq(orderId), ContinueOrderWorkflowRes::class.java).state

    suspend fun rebuildProjection(orderId: String): OrderState =
        request(orderId, RebuildOrderProjectionReq(orderId), RebuildOrderProjectionRes::class.java).state

    private suspend fun <TReply> request(orderId: String, payload: Any, replyType: Class<TReply>): TReply {
        return routes.requestToSpot(orderId, payload)
            .instanceSpot(SampleNames.OrderWorkflowSpotType)
            .inMesh(SampleNames.OrderWorkflowMesh)
            .timeout(SampleTimings.WorkflowTimeout)
            .submit(replyType)
            .await()
    }
}
