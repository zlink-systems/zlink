package systems.zlink.samples.kotlin.bingo.server.play.infrastructure.zlink.actors

import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.kotlin.ZLinkSuspendingActorFactory

class PlayerActorFactory() : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor =
        PlayerActor(context.actorId(), context)
}
