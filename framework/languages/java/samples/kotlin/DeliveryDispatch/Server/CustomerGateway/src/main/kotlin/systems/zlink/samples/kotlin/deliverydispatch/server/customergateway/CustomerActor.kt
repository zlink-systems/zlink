package systems.zlink.samples.kotlin.deliverydispatch.server.customergateway

import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext

class CustomerActor(
    private val id: String,
    private val actorContext: ZLinkActorContext,
) : ZLinkActor {
    fun actorId(): String = id

    override fun context(): ZLinkActorContext = actorContext

    fun push(message: Any) {
        actorContext.boundSession()
            .send(message)
            .submit()
    }
}
