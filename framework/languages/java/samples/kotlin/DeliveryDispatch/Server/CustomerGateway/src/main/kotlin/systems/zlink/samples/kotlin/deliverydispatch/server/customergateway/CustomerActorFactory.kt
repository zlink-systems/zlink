package systems.zlink.samples.kotlin.deliverydispatch.server.customergateway

import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.kotlin.ZLinkSuspendingActorFactory

class CustomerActorFactory : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor =
        CustomerActor(context.actorId(), context)
}
