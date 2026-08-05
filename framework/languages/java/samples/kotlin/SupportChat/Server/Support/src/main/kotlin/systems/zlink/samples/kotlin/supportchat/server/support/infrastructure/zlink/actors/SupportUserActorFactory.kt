package systems.zlink.samples.kotlin.supportchat.server.support.infrastructure.zlink.actors

import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.kotlin.ZLinkSuspendingActorFactory

class SupportUserActorFactory : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(
        context: ZLinkActorContext,
    ): ZLinkActor = SupportUserActor(context.actorId(), context)
}
