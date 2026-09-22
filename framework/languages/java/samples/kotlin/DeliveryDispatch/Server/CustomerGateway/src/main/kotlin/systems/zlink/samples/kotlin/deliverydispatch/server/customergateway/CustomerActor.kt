package systems.zlink.samples.kotlin.deliverydispatch.server.customergateway

import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.kotlin.kotlin

class CustomerActor(private val id: String, private val actorContext: ZLinkActorContext) :
    ZLinkActor {
    fun actorId(): String = id

    override fun context(): ZLinkActorContext = actorContext

    // --8<-- [start:doc-dd-bound-session-push]
    suspend fun push(message: Any) {
        try {
            actorContext.boundSession().kotlin().send(message).await()
        } catch (_: RuntimeException) {
            // Delivery notifications are best effort when the customer session is stale.
        }
    }
    // --8<-- [end:doc-dd-bound-session-push]
}
