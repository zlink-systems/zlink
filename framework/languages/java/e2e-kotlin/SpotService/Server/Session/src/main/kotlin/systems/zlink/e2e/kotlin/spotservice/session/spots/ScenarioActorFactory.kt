package systems.zlink.e2e.kotlin.spotservice.session.spots

import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.kotlin.ZLinkSuspendingActorFactory

class ScenarioActorFactory : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor =
        ScenarioActor(context.actorId(), context)
}
