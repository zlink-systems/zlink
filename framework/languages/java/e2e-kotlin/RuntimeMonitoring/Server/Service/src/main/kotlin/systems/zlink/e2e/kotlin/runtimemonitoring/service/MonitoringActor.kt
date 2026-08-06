package systems.zlink.e2e.kotlin.runtimemonitoring.service

import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.kotlin.ZLinkSuspendingActorFactory

class MonitoringActor(
    private val id: String,
    private val actorContext: ZLinkActorContext,
) : ZLinkActor {
    override fun context(): ZLinkActorContext = actorContext
    fun actorId(): String = id
}

class MonitoringActorFactory : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor =
        MonitoringActor(context.actorId(), context)
}
