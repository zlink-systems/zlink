package systems.zlink.e2e.kotlin.automaticturn

import systems.zlink.framework.kotlin.ZLinkSuspendingSession
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher
import systems.zlink.framework.streams.ZLinkStreamError

class KotlinProbeSession(
    private val sessionContext: ZLinkSessionContext,
    private val handlers: ZLinkSessionPacketDispatcher<ZLinkSessionContext>,
) : ZLinkSuspendingSession() {
    override fun context(): ZLinkSessionContext = sessionContext

    override suspend fun onErrorSuspending(error: ZLinkStreamError) {
        error("stream error: ${error.error()}")
    }

    override suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        val handled = handlers.tryHandle(sessionContext, dispatch, payload).await()
        if (!handled) {
            val actorId = dispatch.metadata()["actor-id"]
            val actor = if (!actorId.isNullOrBlank()) {
                sessionContext.actors().find(actorId).orElseThrow {
                    IllegalStateException("actor is not bound: $actorId")
                }
            } else {
                sessionContext.actors().bound().singleOrNull()
                    ?: error("actor-id metadata is required")
            }
            actor.relay(dispatch, payload).await()
        }
    }

    override suspend fun onActorBindingReplacedSuspending(actorId: String) {
        sessionContext.client()
            .send(Contracts.ActorBindingReplacedNotice(actorId))
            .submit()
            .await()
    }
}
