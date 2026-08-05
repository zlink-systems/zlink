package systems.zlink.samples.kotlin.deliverydispatch.server.customergateway.handlers

import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.actors.ActorRefSnapshot
import systems.zlink.samples.kotlin.deliverydispatch.server.configuration.SampleNames
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.EnsureCustomerActorRes
import systems.zlink.samples.kotlin.deliverydispatch.shared.contracts.EnsureCustomerActorReq

@ZLinkHandlerGroup("customer-route")
class EnsureCustomerActorHandler(
    private val actors: ZLinkActorManager,
) : ZLinkSuspendingRequestHandler<EnsureCustomerActorReq, EnsureCustomerActorRes> {
    override suspend fun handle(
        request: EnsureCustomerActorReq,
        context: ZLinkMessageContext,
    ): EnsureCustomerActorRes {
        val actor = actors.kotlin()
            .getOrCreate(request.customerId, SampleNames.CustomerActorType)
            .request(request)
            .await()
        return EnsureCustomerActorRes(
            customerId = request.customerId,
            actorRef = ActorRefSnapshot.from(requireActor(actor)),
        )
    }

    private fun requireActor(result: ZLinkActorCreateResult): ActorRef = when (result) {
        is ZLinkActorCreateResult.Existing -> result.actor
        is ZLinkActorCreateResult.Created -> result.actor
        is ZLinkActorCreateResult.Rejected -> throw IllegalStateException("Customer actor creation was rejected.")
    }
}
