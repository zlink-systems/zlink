package systems.zlink.samples.kotlin.tictactoe.server.play.infrastructure.zlink.actors

import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.kotlin.ZLinkSuspendingActorFactory

class PlayActorFactory() : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(
        context: ZLinkActorContext,
    ): ZLinkActor =
        PlayActor(context.actorId(), context)
}
